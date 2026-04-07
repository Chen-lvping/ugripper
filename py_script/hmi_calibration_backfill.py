#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

import hmi_sn_batch_writer as writer


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="批量补写夹爪标定：遍历当前串口，读取已写入 SN，若在 ./ugripper_calib/<sn>/ 找到标定源则直接补写并读回校验。"
    )
    parser.add_argument(
        "--calib-root",
        default=None,
        help="默认使用脚本所在目录下的 ./ugripper_calib",
    )
    parser.add_argument(
        "--ports",
        nargs="*",
        default=[],
        help="可选：只处理指定串口；未指定时自动遍历系统串口",
    )
    parser.add_argument(
        "--require-sn-prefix",
        default="",
        help="可选：仅处理 SN 以该前缀开头的夹爪",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    calib_root = Path(args.calib_root).expanduser().resolve() if args.calib_root else script_dir / "ugripper_calib"
    ports = [str(Path(port).expanduser()) for port in args.ports] if args.ports else writer.iter_candidate_ports()
    prefix = writer.normalize_sn(args.require_sn_prefix) if args.require_sn_prefix else ""

    writer.print_info(f"标定搜索目录: {calib_root}")
    writer.print_info(f"待检查串口数: {len(ports)}")

    success_count = 0
    skipped_count = 0
    failed_count = 0

    for port in ports:
        print()
        writer.print_info(f"检查串口: {port}")
        try:
            current_sn = writer.helper_read_sn(port)
        except Exception as exc:
            writer.print_warn(f"跳过，无法读取 SN: {exc}")
            skipped_count += 1
            continue

        print(f"  当前 SN: {current_sn}")
        if prefix and not current_sn.startswith(prefix):
            writer.print_warn(f"  SN 前缀不匹配，跳过。要求前缀: {prefix}")
            skipped_count += 1
            continue

        calibration_source = writer.locate_calibration_source(current_sn, calib_root)
        if calibration_source is None:
            writer.print_warn("  未找到对应标定源，跳过。")
            skipped_count += 1
            continue

        print(f"  标定源: {calibration_source}")
        try:
            calibration_payload = writer.build_calibration_bin(calibration_source)
            writer.write_calibration_with_verify(port, current_sn, calibration_payload)
            writer.print_success(f"  标定补写成功: port={port} sn={current_sn}")
            success_count += 1
        except Exception as exc:
            writer.print_error(f"  标定补写失败: port={port} sn={current_sn} reason={exc}")
            failed_count += 1

    print()
    writer.print_info(
        f"完成。success={success_count} skipped={skipped_count} failed={failed_count}"
    )
    return 1 if failed_count > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
