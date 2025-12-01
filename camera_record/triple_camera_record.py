#!/usr/bin/env python3
"""

特点:
1. 使用 PyAV 替代 ffmpeg 命令，纯 Python 实现
2. 多线程并发录制
3. 使用流拷贝 (Stream Copy) 模式，降低 CPU 占用
4. 统一系统时间戳，确保同步
"""
import av
import threading
import time
import sys
import os
import csv
import argparse
from datetime import datetime
from threading import Barrier
from fractions import Fraction

class TripleCameraRecorder:
    def __init__(self, output_dir='triple_camera_videos'):
        self.output_dir = output_dir
        os.makedirs(output_dir, exist_ok=True)
        self.stop_event = threading.Event()
        self.start_event = threading.Event() # 全局启动信号
        self.start_system_time = 0.0         # 全局启动时间 (系统时间)
        self.threads = []
        self.barrier = None

    def record_camera(self, config):
        video_idx = config['index']
        output_file = config['output']
        csv_file = os.path.splitext(output_file)[0] + '.csv' # 生成 CSV 文件名
        width = config['width']
        height = config['height']
        fps = config['fps']
        duration = config['duration']
        
        print(f'[Camera {video_idx}] 初始化...')
        
        input_container = None
        output_container = None
        csv_f = None
        
        try:
            # 打开输入设备
            # 使用 v4l2 格式，并指定 MJPEG 以便进行流拷贝
            options = {
                'framerate': str(fps),
                'video_size': f'{width}x{height}',
                'input_format': 'mjpeg',
            }
            
            # 打开摄像头
            input_container = av.open(f'/dev/video{video_idx}', format='v4l2', options=options)
            input_stream = input_container.streams.video[0]
            
            # 检查实际打开的分辨率
            actual_width = input_stream.width
            actual_height = input_stream.height
            if actual_width != width or actual_height != height:
                print(f'[Camera {video_idx}] 警告: 请求 {width}x{height}, 实际 {actual_width}x{actual_height}')
            
            # 打开输出文件
            output_container = av.open(output_file, 'w')
            
            # 打开 CSV 文件
            csv_f = open(csv_file, 'w', newline='', buffering=1)
            csv_writer = csv.writer(csv_f)
            csv_writer.writerow(['Frame', 'PTS_us', 'SystemTime_s'])
            
            # 添加输出流 (MJPEG)
            output_stream = output_container.add_stream('mjpeg', rate=fps)
            output_stream.width = actual_width
            output_stream.height = actual_height
            output_stream.pix_fmt = 'yuvj420p'
            # 关键：设置时间基准为微秒 (1/1000000)，方便映射系统时间
            output_stream.time_base = Fraction(1, 1000000)
            
            print(f'[Camera {video_idx}] 就绪，等待同步...')
            
            # 1. 等待所有线程准备好
            if self.barrier:
                try:
                    self.barrier.wait()
                except threading.BrokenBarrierError:
                    return
            
            print(f'[Camera {video_idx}] 缓冲区清空模式...')
            
            frames_count = 0
            has_started_log = False
            
            # 录制循环
            for packet in input_container.demux(input_stream):
                if self.stop_event.is_set():
                    break
                
                if packet.pts is None:
                    continue
                
                # 等待全局启动信号，在此之前丢弃所有帧以清空缓冲区
                if not self.start_event.is_set():
                    continue
                
                if not has_started_log:
                    print(f'[Camera {video_idx}] 同步启动! 开始写入...')
                    has_started_log = True
                
                # 3. 获取当前系统时间 (接收时间)
                current_time = time.time()
                
                # 4. 计算相对时间 (微秒)
                # 公式: (当前系统时间 - 全局启动时间) * 1000000
                rel_pts = int((current_time - self.start_system_time) * 1000000)
                
                # 过滤掉启动前的帧 (理论上很少)
                if rel_pts < 0:
                    continue
                
                # 检查时长
                if rel_pts > duration * 1000000:
                    break
                
                # 5. 重写时间戳 (使用系统时间)
                packet.dts = rel_pts
                packet.pts = rel_pts
                packet.stream = output_stream
                
                # 写入文件
                output_container.mux(packet)
                
                # 写入 CSV
                csv_writer.writerow([frames_count, rel_pts, f'{current_time:.6f}'])
                
                frames_count += 1
                
            print(f'[Camera {video_idx}] 录制结束. 帧数: {frames_count}')
            
        except Exception as e:
           
            print(f'[Camera {video_idx}] 错误: {e}')
            
            if self.barrier:
                self.barrier.abort() # 通知其他线程退出
        finally:
            if csv_f:
                csv_f.close()
            if input_container:
                input_container.close()
            if output_container:
                output_container.close()

    def start_all(self, configs, duration):
        print('='*80)
        print('三相机同步录制 (PyAV - 系统时间戳版)')
        print('='*80)
        print(f'输出目录: {self.output_dir}')
        print(f'录制时长: {duration} 秒')
        print('='*80)
        
        # 更新配置中的时长
        for config in configs:
            config['duration'] = duration
            
        # 创建同步屏障 (N个相机线程 + 1个主线程)
        self.barrier = Barrier(len(configs) + 1)
        self.start_event.clear()
        
        self.threads = []
        for config in configs:
            t = threading.Thread(target=self.record_camera, args=(config,))
            t.start()
            self.threads.append(t)
            
        print('主线程: 等待相机初始化...')
        try:
            self.barrier.wait(timeout=10) # 增加超时
        except threading.BrokenBarrierError:
            print('错误: 相机初始化超时或失败')
            self.stop_event.set()
            # 等待线程清理
            for t in self.threads:
                t.join(timeout=1)
            return

        print('主线程: 等待缓冲区清空 (2秒)...')
        time.sleep(2)

        # 记录全局启动时间 (系统时间)
        self.start_system_time = time.time()
        self.start_event.set() # 发令枪：通知所有线程开始写入
        
        print(f'所有相机同步启动! 系统时间: {datetime.fromtimestamp(self.start_system_time)}')
        
        # 监控录制进度
        try:
            while True:
                elapsed = time.time() - self.start_system_time
                
                # 检查是否所有线程都已结束
                if not any(t.is_alive() for t in self.threads):
                    break
                    
                # 超时保护 (比预定时间多给5秒缓冲)
                if elapsed > duration + 5:
                    print('\n超时，强制停止...')
                    self.stop_event.set()
                    break
                
                print(f'\r录制中: {elapsed:.1f}/{duration}s', end='', flush=True)
                time.sleep(0.5)
                
        except KeyboardInterrupt:
            print('\n\n收到停止信号...')
            self.stop_event.set()
            
        print('\n\n等待线程结束...')
        for t in self.threads:
            t.join()
            
        print('\n录制完成')
        self.verify_videos(configs)
        
    def verify_videos(self, configs):
        print('\n验证视频文件:')
        print('-' * 60)
        for config in configs:
            path = config['output']
            if os.path.exists(path):
                try:
                    with av.open(path) as container:
                        stream = container.streams.video[0]
                        duration_sec = float(stream.duration * stream.time_base) if stream.duration else 0
                        print(f'Camera {config["index"]}: ✓ {stream.width}x{stream.height} @ {stream.average_rate}fps')
                        print(f'  - 帧数: {stream.frames}')
                        print(f'  - 时长: {duration_sec:.2f}s')
                        print(f'  - 路径: {path}')
                except Exception as e:
                    print(f'Camera {config["index"]}:无法读取 ({e})')
            else:
                print(f'Camera {config["index"]}:文件不存在')

def main():
    parser = argparse.ArgumentParser(description='三相机同步录制工具 (PyAV版)')
    parser.add_argument('-d', '--duration', type=int, required=True, help='录制时长(秒)')
    parser.add_argument('-o', '--output', type=str, default='raw_data', help='输出目录')
    parser.add_argument('--device-id', type=str, default='ugripper_001', help='设备ID')
    
    args = parser.parse_args()
    
    # 生成 episode 目录: episode_<date>_<deviceid>_<id>
    date_str = datetime.now().strftime('%Y%m%d')
    
    # 自动获取下一个 episode ID
    os.makedirs(args.output, exist_ok=True)
    existing = [d for d in os.listdir(args.output) 
               if d.startswith('episode_') and os.path.isdir(os.path.join(args.output, d))]
    episode_id = 0
    for d in existing:
        parts = d.split('_')
        if len(parts) >= 4:
            try:
                episode_id = max(episode_id, int(parts[-1]) + 1)
            except ValueError:
                pass
    
    output_dir = os.path.join(args.output, f'episode_{date_str}_{args.device_id}_{episode_id:04d}')

    # 相机配置
    configs = [
        {
            'index': 0,
            'width': 1920,
            'height': 1080,
            'fps': 60,
            'name': 'cam',
            'output': os.path.join(output_dir, 'cam.mkv')
        },
        {
            'index': 2,
            'width': 640,
            'height': 480,
            'fps': 120,
            'name': 'tact_left',
            'output': os.path.join(output_dir, 'tact_left.mkv')
        },
        {
            'index': 4,
            'width': 640,
            'height': 480,
            'fps': 120,
            'name': 'tact_right',
            'output': os.path.join(output_dir, 'tact_right.mkv')
        }
    ]
    
    recorder = TripleCameraRecorder(output_dir)
    recorder.start_all(configs, args.duration)

if __name__ == '__main__':
    main()
