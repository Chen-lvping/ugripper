import json
import datetime
import argparse
import struct
from collections import defaultdict


def as_vector(data, key, size, default):
    value = data.get(key, default)

    if isinstance(value, dict):
        if size == 4:
            seq = [
                value.get("x", 0),
                value.get("y", 0),
                value.get("z", 0),
                value.get("w", 1),
            ]
        else:
            seq = [value.get("x", 0), value.get("y", 0), value.get("z", 0)]
    elif isinstance(value, (list, tuple)):
        seq = list(value)
    else:
        seq = list(default)

    if len(seq) < size:
        seq = seq + [0] * (size - len(seq))

    try:
        return [float(seq[i]) for i in range(size)]
    except Exception:
        return list(default)


def resolve_message_encoding(schema, channel):
    """Pick the most relevant encoding hint for the current message."""
    channel_encoding = getattr(channel, "message_encoding", None) or ""
    schema_encoding = getattr(schema, "encoding", None) if schema else ""
    return channel_encoding or schema_encoding or ""


def is_json_like_encoding(encoding: str) -> bool:
    """Treat empty or 'json*' encodings (e.g. jsonschema) as JSON payloads."""
    if not encoding:
        return True
    normalized = encoding.lower()
    return normalized.startswith("json")


def format_time_delta(log_time_ns, publish_time_ns):
    if publish_time_ns is None:
        return ""
    delta_ms = (publish_time_ns - log_time_ns) / 1e6
    return f" dt={delta_ms:.3f}ms"


def format_payload(data):
    """
    智能格式化 Payload 数据
    根据字段特征自动匹配显示格式：IMU / 编码器 / 通用JSON
    """
    if not isinstance(data, dict):
        return str(data)

    keys = data.keys()

    # --- 1. 适配 IMU 格式 (Foxglove schema / 紧凑schema) ---
    if "linear_acceleration" in keys and "angular_velocity" in keys:
        acc = data.get("linear_acceleration", {})
        gyro = data.get("angular_velocity", {})
        quat = data.get("orientation", {})

        return (
            f"Acc:({acc.get('x', 0):.2f},{acc.get('y', 0):.2f},{acc.get('z', 0):.2f}) "
            f"Gyro:({gyro.get('x', 0):.2f},{gyro.get('y', 0):.2f},{gyro.get('z', 0):.2f}) "
            f"Quat:({quat.get('w', 0):.2f},{quat.get('x', 0):.2f},{quat.get('y', 0):.2f},{quat.get('z', 0):.2f})"
        )

    if "a" in keys and "g" in keys and "q" in keys:
        acc = as_vector(data, "a", 3, [0.0, 0.0, 0.0])
        gyro = as_vector(data, "g", 3, [0.0, 0.0, 0.0])
        quat = as_vector(data, "q", 4, [0.0, 0.0, 0.0, 1.0])

        return (
            f"Acc:({acc[0]:.2f},{acc[1]:.2f},{acc[2]:.2f}) "
            f"Gyro:({gyro[0]:.2f},{gyro[1]:.2f},{gyro[2]:.2f}) "
            f"Quat:({quat[3]:.2f},{quat[0]:.2f},{quat[1]:.2f},{quat[2]:.2f})"
        )

    # --- 2. 适配 编码器 格式 ---
    if "position_raw" in keys and "position_rad" in keys:
        return f"PosRaw: {data['position_raw']:<8} PosRad: {data['position_rad']:.6f}"

    if "r" in keys and "p" in keys:
        return f"PosRaw: {data['r']:<8} PosRad: {data['p']:.6f}"

    # --- 3. 通用格式 (fallback) ---
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def format_binary_payload(topic, payload_bytes, log_time_ns, publish_time_ns):
    time_delta_suffix = format_time_delta(log_time_ns, publish_time_ns)

    if topic == "imu_raw" and len(payload_bytes) >= 40:
        qx, qy, qz, qw, gx, gy, gz, ax, ay, az = struct.unpack(
            "<10f", payload_bytes[:40]
        )
        return (
            f"Acc:({ax:.2f},{ay:.2f},{az:.2f}) "
            f"Gyro:({gx:.2f},{gy:.2f},{gz:.2f}) "
            f"Quat:({qw:.2f},{qx:.2f},{qy:.2f},{qz:.2f})"
            f"{time_delta_suffix}"
        )

    if topic == "encoder" and len(payload_bytes) >= 8:
        raw, rad = struct.unpack("<if", payload_bytes[:8])
        return f"PosRaw: {raw:<8} PosRad: {rad:.6f}{time_delta_suffix}"


    return f"<Binary Data ({len(payload_bytes)} bytes){time_delta_suffix}>"


def read_mcap(file_path, topic_filters=None):
    try:
        from mcap.reader import make_reader
    except ModuleNotFoundError:
        print("Error: python mcap package not found. Please install dependency first.")
        return

    print(f"Opening: {file_path}")

    msg_count = 0
    total_bytes = 0
    start_time = None
    end_time = None
    topic_stats = defaultdict(
        lambda: {
            "count": 0,
            "bytes": 0,
            "start": None,
            "end": None,
        }
    )

    def iter_messages_with_fallback(file_obj):
        reader = make_reader(file_obj)
        yielded = 0
        for item in reader.iter_messages():
            yielded += 1
            yield item

        if yielded > 0:
            return

        file_obj.seek(0)
        from mcap.stream_reader import StreamReader

        schemas = {}
        channels = {}
        for record in StreamReader(file_obj).records:
            name = type(record).__name__
            if name == "Schema":
                schemas[record.id] = record
            elif name == "Channel":
                channels[record.id] = record
            elif name == "Message":
                channel = channels.get(record.channel_id)
                if channel is None:
                    continue
                schema = schemas.get(channel.schema_id)
                yield schema, channel, record

    try:
        with open(file_path, "rb") as f:
            header = f"{'Timestamp':<26} | {'Topic':<20} | {'Data Payload'}"
            print("-" * 120)
            print(header)
            print("-" * 120)

            for schema, channel, message in iter_messages_with_fallback(f):
                if topic_filters and channel.topic not in topic_filters:
                    continue

                msg_count += 1

                msg_size = len(message.data)
                total_bytes += msg_size

                if start_time is None:
                    start_time = message.log_time
                end_time = message.log_time

                dt = datetime.datetime.fromtimestamp(message.log_time / 1e9)
                ts_str = dt.strftime("%Y-%m-%d %H:%M:%S.%f")

                stats = topic_stats[channel.topic]
                stats["count"] += 1
                stats["bytes"] += msg_size
                stats["start"] = (
                    message.log_time
                    if stats["start"] is None
                    else min(stats["start"], message.log_time)
                )
                stats["end"] = message.log_time

                payload_str = ""
                publish_time = getattr(message, "publish_time", None)

                try:
                    encoding = resolve_message_encoding(schema, channel)

                    if is_json_like_encoding(encoding):
                        try:
                            payload_data = json.loads(message.data.decode("utf-8"))
                            payload_str = format_payload(payload_data)
                            payload_str += format_time_delta(message.log_time, publish_time)
                        except Exception:
                            payload_str = f"<Binary Data ({msg_size} bytes)>"
                    else:
                        payload_str = format_binary_payload(
                            channel.topic,
                            message.data,
                            message.log_time,
                            publish_time,
                        )

                except Exception as e:
                    payload_str = f"<Parse Error: {e}>"

                print(f"{ts_str} | {channel.topic:<20} | {payload_str}")

    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return
    except Exception as e:
        print(f"Error reading MCAP: {e}")
        return

    if msg_count > 0:
        print("-" * 120)
        print("Summary:")
        print(f"  Total Messages: {msg_count}")
        print(f"  Total Data:     {total_bytes / 1024:.2f} KB")

        if start_time is not None and end_time is not None:
            duration_ns = end_time - start_time
            duration_sec = duration_ns / 1e9

            print(f"  Duration:       {duration_sec:.2f} seconds")

            if duration_sec > 0:
                freq = msg_count / duration_sec
                bitrate_kbps = (total_bytes * 8) / duration_sec / 1000.0

                print(f"  Avg Frequency:  {freq:.2f} Hz")
                print(f"  Avg Bitrate:    {bitrate_kbps:.2f} kbps")

        if topic_stats:
            print("\nPer-topic summary:")
            summary_header = f"{'Topic':<24} | {'Msgs':>8} | {'Data (KB)':>10} | {'Duration(s)':>12} | {'Hz':>10}"
            print(summary_header)
            print("-" * len(summary_header))
            for topic, stats in topic_stats.items():
                topic_duration = 0.0
                if (
                    stats["start"] is not None
                    and stats["end"] is not None
                    and stats["end"] > stats["start"]
                ):
                    topic_duration = (stats["end"] - stats["start"]) / 1e9
                topic_hz = (
                    (stats["count"] / topic_duration) if topic_duration > 0 else 0.0
                )
                print(
                    f"{topic:<24} | {stats['count']:>8} | {stats['bytes'] / 1024:>10.2f} | {topic_duration:>12.3f} | {topic_hz:>10.2f}"
                )
    else:
        print("No messages found in the file.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="MCAP viewer with topic filter support."
    )
    parser.add_argument("file", help="Path to MCAP file")
    parser.add_argument(
        "-t",
        "--topic",
        action="append",
        help="Topic name to filter (can be used multiple times).",
    )
    args = parser.parse_args()

    filters = set(args.topic) if args.topic else None
    read_mcap(args.file, topic_filters=filters)
