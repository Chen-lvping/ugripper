import sys
import json
import datetime
from mcap.reader import make_reader

def read_imu_mcap(file_path):
    print(f"Opening: {file_path}")
    
    msg_count = 0
    start_time = None
    end_time = None

    try:
        with open(file_path, "rb") as f:
            reader = make_reader(f)
            
            # 打印表头
            header = f"{'Timestamp':<26} | {'Acc(x,y,z)':<24} | {'Gyro(x,y,z)':<24} | {'Quat(w,x,y,z)':<30}"
            print("-" * len(header))
            print(header)
            print("-" * len(header))

            for schema, channel, message in reader.iter_messages():
                msg_count += 1
                
                # 记录时间范围
                if start_time is None:
                    start_time = message.log_time
                end_time = message.log_time

                # 解析 JSON Payload
                # C++ 中存储的是 bytes 类型的 JSON 字符串
                try:
                    payload = message.data.decode('utf-8')
                    data = json.loads(payload)
                    
                    # 提取数据 (根据 foxglove.Imu 结构)
                    # 注意：C++代码中写入的是 standard units (m/s^2, rad/s)
                    acc = data.get("linear_acceleration", {})
                    gyro = data.get("angular_velocity", {})
                    quat = data.get("orientation", {})
                    
                    # 格式化时间戳
                    ts_dt = datetime.datetime.fromtimestamp(message.log_time / 1e9)
                    ts_str = ts_dt.strftime("%Y-%m-%d %H:%M:%S.%f")

                    # 格式化输出字符串
                    acc_str = f"{acc.get('x',0):.2f}, {acc.get('y',0):.2f}, {acc.get('z',0):.2f}"
                    gyro_str = f"{gyro.get('x',0):.2f}, {gyro.get('y',0):.2f}, {gyro.get('z',0):.2f}"
                    quat_str = f"{quat.get('w',0):.2f}, {quat.get('x',0):.2f}, {quat.get('y',0):.2f}, {quat.get('z',0):.2f}"

                    print(f"{ts_str} | {acc_str:<24} | {gyro_str:<24} | {quat_str:<30}")

                except Exception as e:
                    print(f"[Error] Failed to parse message {msg_count}: {e}")

    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return
    except Exception as e:
        print(f"Error reading MCAP: {e}")
        return

    # 打印摘要
    print("-" * len(header))
    print(f"Summary:")
    print(f"  Total Messages: {msg_count}")
    if start_time and end_time:
        duration_sec = (end_time - start_time) / 1e9
        print(f"  Duration:       {duration_sec:.2f} seconds")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 read_mcap.py <path_to_mcap_file>")
    else:
        read_imu_mcap(sys.argv[1])
