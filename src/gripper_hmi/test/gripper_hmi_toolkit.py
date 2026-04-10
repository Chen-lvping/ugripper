#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError as exc:  # pragma: no cover - import guard for delivery environment
    raise SystemExit(
        "缺少 pyserial，请先安装：python3 -m pip install pyserial"
    ) from exc


SERIAL_NUMBER_FIELD_LENGTH = 32
SERIAL_NUMBER_CHUNK_SIZE = 16
CALIBRATION_PAYLOAD_SIZE = 1024
CALIBRATION_CHUNK_SIZE = 16
CALIBRATION_CHUNK_COUNT = CALIBRATION_PAYLOAD_SIZE // CALIBRATION_CHUNK_SIZE
STATUS_FRAME_LENGTH = 8
RAW_DATA_FRAME_HEAD = b"\xA5\xA5"
SEND_FRAME_HEAD = b"\x5A\x5A"

STATUS_OK = 0x54
STATUS_ADDRESS_OUT_OF_LIMIT = 0x88
STATUS_INDEX_ERROR = 0xF1
STATUS_FUNCTION_ERROR = 0xF2
STATUS_MISSING_DATA = 0xF3
STATUS_INVALID_COMMAND = 0xF4
STATUS_ZERO_DATA_CHECKSUM_ERROR = 0xFE
STATUS_CHECKSUM_ERROR = 0xFF

READ_SN_CMD = bytes.fromhex("5A5A5253000001")
WRITE_SN_CMD = bytes.fromhex("5A5A57534E4309")
BEGIN_CALIB_WRITE_CMD = bytes.fromhex("5A5A575A726F10")
ABORT_CALIB_WRITE_CMD = bytes.fromhex("5A5A41626F72745772697465496E446174610000")
END_CALIB_WRITE_CMD = bytes.fromhex("5A5A53746F705F5772697465496E44617461001E")

EXCLUSIVE_COMMAND_TIMEOUT_S = 0.5
SERIAL_TAIL_TIMEOUT_S = 0.12
CALIBRATION_RANGE_READ_TIMEOUT_S = 6.5
CALIBRATION_BEGIN_SETTLE_S = 0.05
CALIBRATION_CHUNK_SEND_INTERVAL_S = 0.02
CALIBRATION_ABORT_DRAIN_S = 0.12
CALIBRATION_ABORT_SETTLE_S = 0.08
CALIBRATION_COMMIT_DELAY_S = 1.5
EXCLUSIVE_DRAIN_IDLE_S = 0.04
EXCLUSIVE_DRAIN_MAX_S = 0.12
SERIAL_NUMBER_COMMAND_RETRY_LIMIT = 3
CALIBRATION_BEGIN_RETRY_LIMIT = 5
CALIBRATION_CHUNK_RETRY_LIMIT = 8
CALIBRATION_READ_RETRY_LIMIT = 5
CALIBRATION_ABORT_RETRY_LIMIT = 2


def calculate_xor(data: bytes) -> int:
    if not data:
        return 0
    value = data[0]
    for byte in data[1:]:
        value ^= byte
    return value


def hex_encode(data: bytes) -> str:
    return data.hex()


def describe_status_code(code: int) -> str:
    mapping = {
        STATUS_OK: "ok",
        STATUS_ADDRESS_OUT_OF_LIMIT: "address_out_of_limit",
        STATUS_INDEX_ERROR: "index_error",
        STATUS_FUNCTION_ERROR: "function_error",
        STATUS_MISSING_DATA: "missing_data",
        STATUS_INVALID_COMMAND: "invalid_command",
        STATUS_ZERO_DATA_CHECKSUM_ERROR: "zero_data_checksum_error",
        STATUS_CHECKSUM_ERROR: "checksum_error",
    }
    return mapping.get(code, f"unknown_status_0x{code:02X}")


def describe_status_frame(token: int, code: int) -> str:
    return f"status token=0x{token:02X} code=0x{code:02X} ({describe_status_code(code)})"


def is_retryable_calibration_status(status_code: int) -> bool:
    return status_code in {
        STATUS_MISSING_DATA,
        STATUS_ZERO_DATA_CHECKSUM_ERROR,
        STATUS_CHECKSUM_ERROR,
        STATUS_ADDRESS_OUT_OF_LIMIT,
    }


def is_calibration_abort_recovery_status(status_code: int) -> bool:
    return status_code in {STATUS_MISSING_DATA, STATUS_ZERO_DATA_CHECKSUM_ERROR}


def is_calibration_write_ack_token(packet_index: int, token: int) -> bool:
    return token == packet_index or token == (packet_index + 1) & 0xFF


def is_exclusive_status_frame(frame: bytes) -> bool:
    return (
        len(frame) == STATUS_FRAME_LENGTH
        and frame[:2] == RAW_DATA_FRAME_HEAD
        and calculate_xor(frame[:-1]) == frame[-1]
        and frame[4:7] == b"\x00\x00\xA5"
    )


def validate_calibration_read_frame(frame: bytes) -> bool:
    return (
        len(frame) == 20
        and frame[:2] == RAW_DATA_FRAME_HEAD
        and calculate_xor(frame[3:19]) == frame[19]
    )


def encode_serial_number(serial_number: str) -> bytes:
    if not serial_number or len(serial_number) > SERIAL_NUMBER_FIELD_LENGTH:
        raise RuntimeError(
            f"invalid serial number, expected 1..{SERIAL_NUMBER_FIELD_LENGTH} printable ASCII characters"
        )
    encoded = serial_number.encode("ascii")
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise RuntimeError("invalid serial number, only printable ASCII is allowed")
    return encoded.ljust(SERIAL_NUMBER_FIELD_LENGTH, b"\x00")


def decode_serial_number(encoded: bytes) -> str:
    serial_number = encoded.rstrip(b"\x00").decode("ascii", errors="ignore")
    if len(serial_number) == 32 and serial_number[:16] == serial_number[16:]:
        return serial_number[:16]
    return serial_number


def build_write_sn_frame(chunk: bytes) -> bytes:
    payload = SEND_FRAME_HEAD + chunk.ljust(SERIAL_NUMBER_CHUNK_SIZE, b"\x00")
    return payload + bytes([calculate_xor(payload)])


def build_read_calibration_chunk_command(packet_index: int) -> bytes:
    payload = bytes([0x5A, 0x5A, 0x52, 0x5A, 0x00, packet_index & 0xFF])
    return payload + bytes([calculate_xor(payload)])


def build_calib_chunk_frame(packet_index: int, chunk: bytes) -> bytes:
    payload = SEND_FRAME_HEAD + bytes([packet_index & 0xFF]) + chunk.ljust(CALIBRATION_CHUNK_SIZE, b"\x00")
    return payload + bytes([calculate_xor(payload)])


@dataclass
class ClientOptions:
    port: str
    baudrate: int = 115200
    log_callback: Optional[Callable[[str], None]] = None


@dataclass
class SerialWriteAckSummary:
    preamble_token: int
    preamble_status: int
    chunk0_token: int
    chunk0_status: int
    chunk1_token: int
    chunk1_status: int

    def to_text(self) -> str:
        return (
            f"preamble={describe_status_frame(self.preamble_token, self.preamble_status)}; "
            f"chunk0={describe_status_frame(self.chunk0_token, self.chunk0_status)}; "
            f"chunk1={describe_status_frame(self.chunk1_token, self.chunk1_status)}"
        )


class GripperHmiClient:
    def __init__(self, options: ClientOptions):
        self.options = options
        self.ser: Optional[serial.Serial] = None
        self.last_error = ""
        self.last_serial_write_ack_summary: Optional[SerialWriteAckSummary] = None

    def __enter__(self) -> "GripperHmiClient":
        self.open()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def log(self, message: str) -> None:
        if self.options.log_callback is not None:
            self.options.log_callback(message)

    def open(self) -> None:
        if self.ser is not None:
            return
        serial_kwargs = dict(
            port=self.options.port,
            baudrate=self.options.baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0,
            write_timeout=0.5,
        )
        if os.name != "nt":
            serial_kwargs["exclusive"] = True
        self.ser = serial.Serial(**serial_kwargs)
        self.last_error = ""

    def close(self) -> None:
        if self.ser is not None:
            self.ser.close()
            self.ser = None

    def _require_serial(self) -> serial.Serial:
        if self.ser is None:
            self.open()
        assert self.ser is not None
        return self.ser

    def flush_input(self) -> None:
        ser = self._require_serial()
        ser.reset_input_buffer()

    def drain_quiet_window(self) -> None:
        ser = self._require_serial()
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        quiet_deadline = time.monotonic() + EXCLUSIVE_DRAIN_IDLE_S
        max_deadline = time.monotonic() + EXCLUSIVE_DRAIN_MAX_S
        while time.monotonic() < quiet_deadline and time.monotonic() < max_deadline:
            data = ser.read(256)
            if data:
                self.log(f"rx_drain {hex_encode(data)}")
                quiet_deadline = time.monotonic() + EXCLUSIVE_DRAIN_IDLE_S
            else:
                time.sleep(0.005)

    def write_bytes(self, data: bytes, label: str) -> None:
        ser = self._require_serial()
        self.log(f"tx {label} {hex_encode(data)}")
        written = ser.write(data)
        ser.flush()
        if written != len(data):
            raise RuntimeError(f"short write for {label}: {written}/{len(data)}")

    def read_available(self, timeout_s: float) -> bytes:
        ser = self._require_serial()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                self.log(f"rx_raw {hex_encode(chunk)}")
                return chunk
            time.sleep(0.005)
        return b""

    def _read_expected_status_frame(
        self,
        expected_token: Optional[int],
        timeout_s: float,
    ) -> Optional[tuple[int, int]]:
        deadline = time.monotonic() + timeout_s
        buffer = bytearray()
        while time.monotonic() < deadline:
            chunk = self.read_available(max(0.01, deadline - time.monotonic()))
            if chunk:
                buffer.extend(chunk)
            while len(buffer) >= 2:
                pos = buffer.find(RAW_DATA_FRAME_HEAD)
                if pos < 0:
                    if len(buffer) > 1:
                        del buffer[:-1]
                    break
                if pos > 0:
                    del buffer[:pos]
                if len(buffer) < STATUS_FRAME_LENGTH:
                    break
                frame = bytes(buffer[:STATUS_FRAME_LENGTH])
                if calculate_xor(frame[:-1]) != frame[-1]:
                    del buffer[0]
                    continue
                del buffer[:STATUS_FRAME_LENGTH]
                if not is_exclusive_status_frame(frame):
                    continue
                token = frame[2]
                status_code = frame[3]
                if expected_token is not None and token != expected_token:
                    continue
                self.log(f"rx_status {hex_encode(frame)}")
                return token, status_code
        return None

    def read_status_frame(self, expected_token: int, timeout_s: float) -> Optional[tuple[int, int]]:
        return self._read_expected_status_frame(expected_token, timeout_s)

    def read_any_status_frame(self, timeout_s: float) -> Optional[tuple[int, int]]:
        return self._read_expected_status_frame(None, timeout_s)

    def read_raw_data_or_status(
        self,
        payload_length: int,
        timeout_s: float,
    ) -> tuple[str, bytes | tuple[int, int]]:
        deadline = time.monotonic() + timeout_s
        buffer = bytearray()
        data_frame_size = 2 + payload_length + 1
        while time.monotonic() < deadline:
            chunk = self.read_available(max(0.01, deadline - time.monotonic()))
            if chunk:
                buffer.extend(chunk)
            while len(buffer) >= 2:
                pos = buffer.find(RAW_DATA_FRAME_HEAD)
                if pos < 0:
                    if len(buffer) > 1:
                        del buffer[:-1]
                    break
                if pos > 0:
                    del buffer[:pos]
                if len(buffer) >= data_frame_size and calculate_xor(buffer[: data_frame_size - 1]) == buffer[data_frame_size - 1]:
                    frame = bytes(buffer[:data_frame_size])
                    self.log(f"rx_data {hex_encode(frame)}")
                    return "data", frame[2:2 + payload_length]
                if len(buffer) >= STATUS_FRAME_LENGTH:
                    frame = bytes(buffer[:STATUS_FRAME_LENGTH])
                    if is_exclusive_status_frame(frame):
                        del buffer[:STATUS_FRAME_LENGTH]
                        self.log(f"rx_status {hex_encode(frame)}")
                        return "status", (frame[2], frame[3])
                    if calculate_xor(frame[:-1]) == frame[-1]:
                        del buffer[:STATUS_FRAME_LENGTH]
                        continue
                if len(buffer) > data_frame_size:
                    del buffer[0]
                    continue
                break
        return "timeout", b""

    def read_data_or_status(
        self,
        expected_token: int,
        payload_length: int,
        timeout_s: float,
    ) -> tuple[str, bytes | tuple[int, int]]:
        deadline = time.monotonic() + timeout_s
        buffer = bytearray()
        data_frame_size = 2 + 1 + payload_length + 1
        while time.monotonic() < deadline:
            chunk = self.read_available(max(0.01, deadline - time.monotonic()))
            if chunk:
                buffer.extend(chunk)
            while len(buffer) >= 2:
                pos = buffer.find(RAW_DATA_FRAME_HEAD)
                if pos < 0:
                    if len(buffer) > 1:
                        del buffer[:-1]
                    break
                if pos > 0:
                    del buffer[:pos]
                if (
                    len(buffer) >= data_frame_size
                    and buffer[2] == expected_token
                    and validate_calibration_read_frame(bytes(buffer[:data_frame_size]))
                ):
                    frame = bytes(buffer[:data_frame_size])
                    self.log(f"rx_chunk {hex_encode(frame)}")
                    return "data", frame[3:3 + payload_length]
                if len(buffer) >= STATUS_FRAME_LENGTH:
                    frame = bytes(buffer[:STATUS_FRAME_LENGTH])
                    if is_exclusive_status_frame(frame):
                        del buffer[:STATUS_FRAME_LENGTH]
                        token = frame[2]
                        status_code = frame[3]
                        if token != expected_token:
                            continue
                        self.log(f"rx_status {hex_encode(frame)}")
                        return "status", (token, status_code)
                    if calculate_xor(frame[:-1]) == frame[-1]:
                        del buffer[:STATUS_FRAME_LENGTH]
                        continue
                if len(buffer) > data_frame_size:
                    del buffer[0]
                    continue
                break
        return "timeout", b""

    def abort_calibration_write_state(self, reason: str) -> bool:
        try:
            self.write_bytes(ABORT_CALIB_WRITE_CMD, "abort_calib_write")
            self._read_tail(CALIBRATION_ABORT_DRAIN_S)
            self.flush_input()
            time.sleep(CALIBRATION_ABORT_SETTLE_S)
            self.log(f"abort_calibration_write_state {reason}")
            return True
        except Exception as exc:
            self.last_error = f"failed to abort calibration write state: {exc}"
            return False

    def read_serial_number(self) -> str:
        for attempt in range(SERIAL_NUMBER_COMMAND_RETRY_LIMIT):
            self.flush_input()
            self.write_bytes(READ_SN_CMD, "read_sn_cmd")
            result_kind, result_payload = self.read_raw_data_or_status(
                SERIAL_NUMBER_CHUNK_SIZE,
                EXCLUSIVE_COMMAND_TIMEOUT_S,
            )
            if result_kind == "data":
                encoded = bytearray(SERIAL_NUMBER_FIELD_LENGTH)
                encoded[:SERIAL_NUMBER_CHUNK_SIZE] = result_payload  # type: ignore[index]
                tail_kind, tail_payload = self.read_raw_data_or_status(
                    SERIAL_NUMBER_CHUNK_SIZE,
                    SERIAL_TAIL_TIMEOUT_S,
                )
                if tail_kind == "data":
                    encoded[SERIAL_NUMBER_CHUNK_SIZE:] = tail_payload  # type: ignore[index]
                serial_number = decode_serial_number(bytes(encoded))
                self.last_error = ""
                return serial_number

            if result_kind == "status":
                token, status_code = result_payload  # type: ignore[misc]
                if (
                    attempt + 1 < SERIAL_NUMBER_COMMAND_RETRY_LIMIT
                    and is_calibration_abort_recovery_status(status_code)
                    and self.abort_calibration_write_state(
                        f"serial read recovery after {describe_status_frame(token, status_code)}"
                    )
                ):
                    continue
                self.last_error = f"serial number read failed: {describe_status_frame(token, status_code)}"
                raise RuntimeError(self.last_error)

            status = self.read_status_frame(0x53, EXCLUSIVE_COMMAND_TIMEOUT_S)
            if status is None:
                self.last_error = "timed out waiting for serial number response"
                raise RuntimeError(self.last_error)
            token, status_code = status
            if (
                attempt + 1 < SERIAL_NUMBER_COMMAND_RETRY_LIMIT
                and is_calibration_abort_recovery_status(status_code)
                and self.abort_calibration_write_state(
                    f"serial read status recovery after {describe_status_code(status_code)}"
                )
            ):
                continue
            self.last_error = f"serial number read failed: {describe_status_code(status_code)}"
            raise RuntimeError(self.last_error)

        self.last_error = "serial number read failed after retry"
        raise RuntimeError(self.last_error)

    def write_serial_number(self, serial_number: str) -> SerialWriteAckSummary:
        encoded = encode_serial_number(serial_number)

        def write_chunk_with_retry(chunk_index: int, chunk_data: bytes) -> tuple[int, int]:
            frame = build_write_sn_frame(chunk_data)
            saw_status = False
            last_token = 0
            last_status = 0
            chunk_abort_recoveries = 0
            for attempt in range(CALIBRATION_CHUNK_RETRY_LIMIT):
                self.write_bytes(frame, f"write_sn_chunk{chunk_index}")
                status = self.read_any_status_frame(EXCLUSIVE_COMMAND_TIMEOUT_S)
                if status is None:
                    time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)
                    continue
                saw_status = True
                token, status_code = status
                last_token = token
                last_status = status_code
                if status_code == STATUS_OK:
                    return token, status_code
                if is_calibration_abort_recovery_status(status_code):
                    should_abort_and_recover = status_code == STATUS_MISSING_DATA or (
                        status_code == STATUS_ZERO_DATA_CHECKSUM_ERROR and attempt > 0
                    )
                    if (
                        should_abort_and_recover
                        and chunk_abort_recoveries < CALIBRATION_ABORT_RETRY_LIMIT
                        and self.abort_calibration_write_state(
                            f"serial write recovery chunk={chunk_index} after {describe_status_frame(token, status_code)}"
                        )
                    ):
                        chunk_abort_recoveries += 1
                        continue
                if is_retryable_calibration_status(status_code):
                    time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)
                    continue
                self.last_error = (
                    f"serial number write chunk {chunk_index} failed: "
                    f"{describe_status_frame(token, status_code)}"
                )
                raise RuntimeError(self.last_error)

            self.last_error = (
                f"serial number write chunk {chunk_index} failed after retry"
                + (f": {describe_status_frame(last_token, last_status)}" if saw_status else "")
            )
            raise RuntimeError(self.last_error)

        preamble_ready = False
        saw_preamble_status = False
        last_preamble_token = 0
        last_preamble_status = 0
        preamble_abort_recoveries = 0

        for _ in range(CALIBRATION_BEGIN_RETRY_LIMIT):
            self.flush_input()
            self.write_bytes(WRITE_SN_CMD, "write_sn_preamble")
            status = self.read_any_status_frame(EXCLUSIVE_COMMAND_TIMEOUT_S)
            if status is None:
                continue
            saw_preamble_status = True
            token, status_code = status
            last_preamble_token = token
            last_preamble_status = status_code
            if status_code == STATUS_OK or (token == 0x53 and status_code == STATUS_MISSING_DATA):
                preamble_ready = True
                break
            if (
                is_calibration_abort_recovery_status(status_code)
                and preamble_abort_recoveries < CALIBRATION_ABORT_RETRY_LIMIT
                and self.abort_calibration_write_state(
                    f"serial write preamble recovery after {describe_status_frame(token, status_code)}"
                )
            ):
                preamble_abort_recoveries += 1
                continue
            if is_retryable_calibration_status(status_code):
                time.sleep(CALIBRATION_BEGIN_SETTLE_S)
                continue
            self.last_error = (
                f"serial number write preamble failed: {describe_status_frame(token, status_code)}"
            )
            raise RuntimeError(self.last_error)

        if not preamble_ready:
            self.last_error = (
                "timed out waiting for serial number write preamble acknowledgement"
                + (
                    f": {describe_status_frame(last_preamble_token, last_preamble_status)}"
                    if saw_preamble_status
                    else ""
                )
            )
            raise RuntimeError(self.last_error)

        chunk0_token, chunk0_status = write_chunk_with_retry(0, encoded[:SERIAL_NUMBER_CHUNK_SIZE])
        chunk1_token, chunk1_status = write_chunk_with_retry(1, encoded[SERIAL_NUMBER_CHUNK_SIZE:])
        time.sleep(0.15)
        self.last_error = ""
        self.last_serial_write_ack_summary = SerialWriteAckSummary(
            preamble_token=last_preamble_token,
            preamble_status=last_preamble_status,
            chunk0_token=chunk0_token,
            chunk0_status=chunk0_status,
            chunk1_token=chunk1_token,
            chunk1_status=chunk1_status,
        )
        self.log(f"serial_write_ack {self.last_serial_write_ack_summary.to_text()}")
        return self.last_serial_write_ack_summary

    def read_calibration(self) -> bytes:
        def collect_frames(timeout_s: float) -> tuple[bytearray, list[bool], int, int]:
            raw = bytearray(CALIBRATION_PAYLOAD_SIZE)
            received = [False] * CALIBRATION_CHUNK_COUNT
            response_token = 0
            status_code = 0
            deadline = time.monotonic() + timeout_s
            buffer = bytearray()
            while time.monotonic() < deadline:
                chunk = self.read_available(max(0.01, deadline - time.monotonic()))
                if chunk:
                    buffer.extend(chunk)
                while len(buffer) >= 2:
                    pos = buffer.find(RAW_DATA_FRAME_HEAD)
                    if pos < 0:
                        if len(buffer) > 1:
                            del buffer[:-1]
                        break
                    if pos > 0:
                        del buffer[:pos]
                    if len(buffer) >= 20 and validate_calibration_read_frame(bytes(buffer[:20])):
                        frame = bytes(buffer[:20])
                        token = frame[2]
                        if token < CALIBRATION_CHUNK_COUNT:
                            raw[token * CALIBRATION_CHUNK_SIZE:(token + 1) * CALIBRATION_CHUNK_SIZE] = frame[3:19]
                            received[token] = True
                        del buffer[:20]
                        continue
                    if len(buffer) >= STATUS_FRAME_LENGTH:
                        frame = bytes(buffer[:STATUS_FRAME_LENGTH])
                        if is_exclusive_status_frame(frame):
                            response_token = frame[2]
                            status_code = frame[3]
                            del buffer[:STATUS_FRAME_LENGTH]
                            continue
                        if calculate_xor(frame[:-1]) == frame[-1]:
                            del buffer[:STATUS_FRAME_LENGTH]
                            continue
                    if len(buffer) < 20:
                        break
                    del buffer[0]
            return raw, received, response_token, status_code

        def request_range_read() -> tuple[bytearray, list[bool], int, int]:
            self.write_bytes(build_read_calibration_chunk_command(0x3F), "read_calib_range")
            return collect_frames(CALIBRATION_RANGE_READ_TIMEOUT_S)

        def request_single_chunk_read(packet_index: int) -> tuple[str, bytes | tuple[int, int]]:
            self.write_bytes(
                build_read_calibration_chunk_command(packet_index),
                f"read_calib_chunk{packet_index}",
            )
            return self.read_data_or_status(
                packet_index,
                CALIBRATION_CHUNK_SIZE,
                EXCLUSIVE_COMMAND_TIMEOUT_S,
            )

        self.flush_input()
        range_read_collected = False
        raw = bytearray(CALIBRATION_PAYLOAD_SIZE)
        received = [False] * CALIBRATION_CHUNK_COUNT
        response_token = 0
        status_code = 0
        for attempt in range(CALIBRATION_READ_RETRY_LIMIT):
            raw, received, response_token, status_code = request_range_read()
            if all(received):
                range_read_collected = True
                break
            if (
                not any(received)
                and attempt + 1 < CALIBRATION_READ_RETRY_LIMIT
                and self.abort_calibration_write_state(
                    "range read timeout recovery"
                    if status_code == 0
                    else f"range read recovery after {describe_status_frame(response_token, status_code)}"
                )
            ):
                continue
            if (
                is_calibration_abort_recovery_status(status_code)
                and attempt + 1 < CALIBRATION_READ_RETRY_LIMIT
                and self.abort_calibration_write_state(
                    f"range read recovery after {describe_status_frame(response_token, status_code)}"
                )
            ):
                continue
            if not is_retryable_calibration_status(status_code):
                break
            self.flush_input()
            time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)

        need_sequential_fallback = (
            not range_read_collected and not all(received) and is_retryable_calibration_status(status_code)
        )
        if need_sequential_fallback:
            self.flush_input()
            raw = bytearray(CALIBRATION_PAYLOAD_SIZE)
            received = [False] * CALIBRATION_CHUNK_COUNT
            response_token = 0
            status_code = 0
            for packet_index in range(CALIBRATION_CHUNK_COUNT):
                chunk_received = False
                result_kind = "timeout"
                result_payload: bytes | tuple[int, int] = b""
                for attempt in range(CALIBRATION_CHUNK_RETRY_LIMIT):
                    result_kind, result_payload = request_single_chunk_read(packet_index)
                    if result_kind == "data":
                        payload = result_payload  # type: ignore[assignment]
                        if len(payload) == CALIBRATION_CHUNK_SIZE:
                            raw[
                                packet_index * CALIBRATION_CHUNK_SIZE:(packet_index + 1) * CALIBRATION_CHUNK_SIZE
                            ] = payload
                            received[packet_index] = True
                            chunk_received = True
                            break
                    if result_kind == "status":
                        token, status_code = result_payload  # type: ignore[misc]
                        response_token = token
                        if (
                            is_calibration_abort_recovery_status(status_code)
                            and attempt + 1 < CALIBRATION_CHUNK_RETRY_LIMIT
                            and self.abort_calibration_write_state(
                                f"chunk read recovery packet={packet_index} after {describe_status_frame(token, status_code)}"
                            )
                        ):
                            continue
                        if is_retryable_calibration_status(status_code):
                            time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)
                            continue
                    break
                if not chunk_received:
                    if result_kind == "status":
                        self.last_error = (
                            f"calibration read failed at chunk {packet_index}: "
                            f"{describe_status_frame(response_token, status_code)}"
                        )
                    else:
                        self.last_error = f"timed out waiting for calibration chunk {packet_index}"
                    raise RuntimeError(self.last_error)

        for packet_index, ok in enumerate(received):
            if ok:
                continue
            if status_code != 0:
                self.last_error = (
                    f"calibration read failed at chunk {packet_index}: "
                    f"{describe_status_frame(response_token, status_code)}"
                )
            else:
                self.last_error = f"timed out waiting for calibration chunk {packet_index}"
            raise RuntimeError(self.last_error)

        payload = bytes(raw)
        magic, version, payload_size, header_size, valid_fields = struct.unpack("<4sIHHI", payload[:16])
        if magic != b"UCAL":
            self.last_error = "calibration read returned invalid header magic"
            raise RuntimeError(self.last_error)
        if header_size != 32:
            self.last_error = "calibration read returned invalid header size"
            raise RuntimeError(self.last_error)
        if payload_size == 0 or payload_size > CALIBRATION_PAYLOAD_SIZE:
            self.last_error = "calibration read returned invalid payload size"
            raise RuntimeError(self.last_error)
        self.log(
            f"calibration_header magic={magic.decode(errors='ignore')} version=0x{version:08X} "
            f"payload_size={payload_size} header_size={header_size} valid_fields=0x{valid_fields:08X}"
        )
        self.last_error = ""
        return payload

    def write_calibration(self, payload: bytes) -> None:
        if len(payload) != CALIBRATION_PAYLOAD_SIZE:
            raise RuntimeError(f"invalid calibration size: {len(payload)}")

        normalized = bytearray(payload)
        normalized[0:4] = b"UCAL"
        normalized[10:12] = struct.pack("<H", 32)
        trimmed_payload_size = len(normalized)
        while trimmed_payload_size > 0 and normalized[trimmed_payload_size - 1] == 0:
            trimmed_payload_size -= 1
        normalized[8:10] = struct.pack("<H", trimmed_payload_size)

        def wait_for_any_calibration_status(timeout_s: float) -> Optional[tuple[int, int]]:
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                status = self.read_any_status_frame(max(0.01, deadline - time.monotonic()))
                if status is not None:
                    return status
            return None

        def wait_for_calibration_chunk_status(packet_index: int, timeout_s: float) -> Optional[tuple[int, int]]:
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                status = self.read_any_status_frame(max(0.01, deadline - time.monotonic()))
                if status is None:
                    break
                token, status_code = status
                if not is_calibration_write_ack_token(packet_index, token):
                    continue
                return token, status_code
            return None

        self.flush_input()

        begin_ready = False
        saw_begin_status = False
        last_begin_token = 0
        last_begin_status = 0
        begin_abort_recoveries = 0
        for _ in range(CALIBRATION_BEGIN_RETRY_LIMIT):
            self.write_bytes(BEGIN_CALIB_WRITE_CMD, "begin_calib_write")
            status = wait_for_any_calibration_status(EXCLUSIVE_COMMAND_TIMEOUT_S)
            if status is None:
                continue
            saw_begin_status = True
            token, status_code = status
            last_begin_token = token
            last_begin_status = status_code
            if status_code == STATUS_OK:
                begin_ready = True
                time.sleep(CALIBRATION_BEGIN_SETTLE_S)
                break
            if (
                is_calibration_abort_recovery_status(status_code)
                and begin_abort_recoveries < CALIBRATION_ABORT_RETRY_LIMIT
                and self.abort_calibration_write_state(
                    f"begin write recovery after {describe_status_frame(token, status_code)}"
                )
            ):
                begin_abort_recoveries += 1
                continue
            if is_retryable_calibration_status(status_code):
                time.sleep(CALIBRATION_BEGIN_SETTLE_S)
                continue
            self.last_error = f"begin calibration write failed: {describe_status_frame(token, status_code)}"
            raise RuntimeError(self.last_error)

        if not begin_ready:
            self.last_error = (
                "timed out waiting for begin calibration write acknowledgement"
                + (
                    f": {describe_status_frame(last_begin_token, last_begin_status)}"
                    if saw_begin_status
                    else ""
                )
            )
            raise RuntimeError(self.last_error)

        for packet_index in range(CALIBRATION_CHUNK_COUNT):
            frame = build_calib_chunk_frame(
                packet_index,
                bytes(normalized[packet_index * CALIBRATION_CHUNK_SIZE:(packet_index + 1) * CALIBRATION_CHUNK_SIZE]),
            )
            chunk_sent = False
            saw_status = False
            last_token = 0
            last_status = 0
            chunk_abort_recoveries = 0
            for attempt in range(CALIBRATION_CHUNK_RETRY_LIMIT):
                self.write_bytes(frame, f"calib_chunk_{packet_index}")
                status = wait_for_calibration_chunk_status(packet_index, EXCLUSIVE_COMMAND_TIMEOUT_S)
                if status is None:
                    time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)
                    continue
                saw_status = True
                token, status_code = status
                last_token = token
                last_status = status_code
                if status_code == STATUS_OK:
                    chunk_sent = True
                    break
                if is_calibration_abort_recovery_status(status_code):
                    should_abort_and_recover = status_code == STATUS_MISSING_DATA or (
                        status_code == STATUS_ZERO_DATA_CHECKSUM_ERROR and attempt > 0
                    )
                    if (
                        should_abort_and_recover
                        and chunk_abort_recoveries < CALIBRATION_ABORT_RETRY_LIMIT
                        and self.abort_calibration_write_state(
                            f"chunk write recovery packet={packet_index} after {describe_status_frame(token, status_code)}"
                        )
                    ):
                        chunk_abort_recoveries += 1
                        continue
                if is_retryable_calibration_status(status_code):
                    time.sleep(CALIBRATION_CHUNK_SEND_INTERVAL_S)
                    continue
                self.last_error = (
                    f"calibration chunk {packet_index} failed: {describe_status_frame(token, status_code)}"
                )
                raise RuntimeError(self.last_error)
            if not chunk_sent:
                self.last_error = (
                    f"calibration chunk {packet_index} failed after retry"
                    + (f": {describe_status_frame(last_token, last_status)}" if saw_status else "")
                )
                raise RuntimeError(self.last_error)

        self.write_bytes(END_CALIB_WRITE_CMD, "end_calib_write")
        time.sleep(CALIBRATION_COMMIT_DELAY_S)
        self._read_tail(0.08)
        self.last_error = ""

    def _read_tail(self, timeout_s: float) -> bytes:
        ser = self._require_serial()
        deadline = time.monotonic() + timeout_s
        buffer = bytearray()
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                self.log(f"rx_tail {hex_encode(chunk)}")
                buffer.extend(chunk)
            else:
                time.sleep(0.005)
        return bytes(buffer)


def _build_log_callback(path: Optional[str]) -> Optional[Callable[[str], None]]:
    if not path:
        return None
    log_path = Path(path)
    stream = log_path.open("w", encoding="utf-8")

    def _callback(message: str) -> None:
        stream.write(message + "\n")
        stream.flush()

    return _callback


def is_wch_serial_port(port_info: object) -> bool:
    device = str(getattr(port_info, "device", "") or "")
    description = str(getattr(port_info, "description", "") or "")
    manufacturer = str(getattr(port_info, "manufacturer", "") or "")
    product = str(getattr(port_info, "product", "") or "")
    hwid = str(getattr(port_info, "hwid", "") or "")
    combined = " ".join([device, description, manufacturer, product, hwid]).upper()
    if not device.upper().startswith("COM"):
        return False
    if "BLUETOOTH" in combined:
        return False
    if "WCH" in combined and "USB-SERIAL" in combined:
        return True
    return False


def detect_existing_ports(windows_wch_only: bool = False) -> list[str]:
    port_infos = list(serial.tools.list_ports.comports())
    if windows_wch_only and os.name == "nt":
        port_infos = [port for port in port_infos if is_wch_serial_port(port)]
    ports = [str(port.device) for port in port_infos]
    return sorted(set(ports))


def main() -> int:
    parser = argparse.ArgumentParser(description="Standalone pure-Python gripper HMI SN/calibration tool")
    parser.add_argument("--port", help="串口，例如 /dev/ttyCH9344USB8 或 COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--read-sn", action="store_true")
    parser.add_argument("--write-sn")
    parser.add_argument("--read-calib", action="store_true")
    parser.add_argument("--write-calib-bin")
    parser.add_argument("--dump-calib-bin")
    parser.add_argument("--log-file")
    parser.add_argument("--list-ports", action="store_true")
    args = parser.parse_args()

    if args.list_ports:
        for port in detect_existing_ports():
            print(port)
        return 0

    action_count = (
        int(args.read_sn)
        + int(args.write_sn is not None)
        + int(args.read_calib)
        + int(args.write_calib_bin is not None)
    )
    if action_count != 1:
        parser.error("choose exactly one action")
    if not args.port:
        parser.error("--port is required unless using --list-ports")

    client = GripperHmiClient(
        ClientOptions(
            port=args.port,
            baudrate=args.baud,
            log_callback=_build_log_callback(args.log_file),
        )
    )

    try:
        with client:
            if args.read_sn:
                print(client.read_serial_number())
                return 0
            if args.write_sn is not None:
                ack_summary = client.write_serial_number(args.write_sn)
                print("write-sn ok")
                print(f"ack={ack_summary.to_text()}")
                return 0
            if args.read_calib:
                payload = client.read_calibration()
                magic, version, payload_size, header_size, valid_fields = struct.unpack("<4sIHHI", payload[:16])
                print(f"calibration.magic={magic.decode('ascii', errors='ignore')}")
                print(f"calibration.data_format_version=0x{version:08x}")
                print(f"calibration.payload_size={payload_size}")
                print(f"calibration.header_size={header_size}")
                print(f"calibration.valid_fields=0x{valid_fields:08x}")
                if args.dump_calib_bin:
                    Path(args.dump_calib_bin).write_bytes(payload)
                return 0
            payload = Path(args.write_calib_bin).read_bytes()
            client.write_calibration(payload)
            print("write-calib ok")
            return 0
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
