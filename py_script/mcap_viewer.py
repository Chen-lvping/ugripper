import sys
import json
import datetime
from mcap.reader import make_reader

def format_payload(data):
    """
    智能格式化 Payload 数据
    根据字段特征自动匹配显示格式：IMU / 编码器 / 通用JSON
    """
    if not isinstance(data, dict):
        return str(data)

    keys = data.keys()

    # --- 1. 适配 IMU 格式 (Foxglove schema) ---
    if "linear_acceleration" in keys and "angular_velocity" in keys:
        acc = data.get("linear_acceleration", {})
        gyro = data.get("angular_velocity", {})
        quat = data.get("orientation", {})
        
        return (f"Acc:({acc.get('x',0):.2f},{acc.get('y',0):.2f},{acc.get('z',0):.2f}) "
                f"Gyro:({gyro.get('x',0):.2f},{gyro.get('y',0):.2f},{gyro.get('z',0):.2f}) "
                f"Quat:({quat.get('w',0):.2f},{quat.get('x',0):.2f},{quat.get('y',0):.2f},{quat.get('z',0):.2f})")

    # --- 2. 适配 编码器 格式 ---
    if "position_raw" in keys and "position_rad" in keys:
        # 优化显示格式，对齐数值
        return f"PosRaw: {data['position_raw']:<8} PosRad: {data['position_rad']:.6f}"

    # --- 3. 通用格式 (fallback) ---
    return json.dumps(data, ensure_ascii=False, separators=(',', ':'))

def read_mcap(file_path):
    print(f"Opening: {file_path}")
    
    msg_count = 0
    total_bytes = 0  # 累计消息载荷字节数
    start_time = None
    end_time = None

    try:
        with open(file_path, "rb") as f:
            reader = make_reader(f)
            
            # 打印表头
            header = f"{'Timestamp':<26} | {'Topic':<20} | {'Data Payload'}"
            print("-" * 120)
            print(header)
            print("-" * 120)

            for schema, channel, message in reader.iter_messages():
                msg_count += 1
                
                # 统计载荷大小 (Bytes)
                msg_size = len(message.data)
                total_bytes += msg_size

                # 记录时间范围
                if start_time is None:
                    start_time = message.log_time
                end_time = message.log_time

                # 格式化时间戳
                dt = datetime.datetime.fromtimestamp(message.log_time / 1e9)
                ts_str = dt.strftime("%Y-%m-%d %H:%M:%S.%f")

                # 解析 Payload
                payload_str = ""
                try:
                    encoding = schema.encoding if schema else channel.message_encoding
                    
                    # 尝试解析 JSON
                    if encoding == "json" or encoding == "": 
                        try:
                            payload_data = json.loads(message.data.decode('utf-8'))
                            payload_str = format_payload(payload_data)
                        except Exception:
                            # 如果不是 JSON，显示原始大小
                            payload_str = f"<Binary Data ({msg_size} bytes)>"
                    else:
                        payload_str = f"<{encoding} Data ({msg_size} bytes)>"

                except Exception as e:
                    payload_str = f"<Parse Error: {e}>"

                print(f"{ts_str} | {channel.topic:<20} | {payload_str}")

    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return
    except Exception as e:
        print(f"Error reading MCAP: {e}")
        return

    # 打印摘要信息
    if msg_count > 0:
        print("-" * 120)
        print(f"Summary:")
        print(f"  Total Messages: {msg_count}")
        print(f"  Total Data:     {total_bytes / 1024:.2f} KB")
        
        if start_time is not None and end_time is not None:
            duration_ns = end_time - start_time
            duration_sec = duration_ns / 1e9
            
            print(f"  Duration:       {duration_sec:.2f} seconds")
            
            if duration_sec > 0:
                freq = msg_count / duration_sec
                # 码率计算: (Total Bytes * 8 bits) / Duration seconds / 1000 = kbps
                bitrate_kbps = (total_bytes * 8) / duration_sec / 1000.0
                
                print(f"  Avg Frequency:  {freq:.2f} Hz")
                print(f"  Avg Bitrate:    {bitrate_kbps:.2f} kbps")
    else:
        print("No messages found in the file.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 read_mcap.py <path_to_mcap_file>")
    else:
        read_mcap(sys.argv[1])
