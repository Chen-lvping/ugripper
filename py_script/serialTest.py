import serial
import time
import sys
import threading

class SerialLoopbackTest:
    def __init__(self, port='ttyS7', baudrate=9600, timeout=1):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.serial_port = None
        self.is_testing = False
        self.received_data = []
        
    def open_serial(self):
        """打开串口连接"""
        try:
            # 在 Linux 系统上，串口设备通常位于 /dev/ 目录下
            if not self.port.startswith('/dev/'):
                self.port = '/dev/' + self.port
                
            self.serial_port = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.timeout
            )
            
            if self.serial_port.is_open:
                print(f"成功打开串口: {self.port}")
                return True
            else:
                print(f"无法打开串口: {self.port}")
                return False
                
        except serial.SerialException as e:
            print(f"串口错误: {e}")
            return False
        except Exception as e:
            print(f"未知错误: {e}")
            return False
    
    def close_serial(self):
        """关闭串口连接"""
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
            print("串口已关闭")
    
    def send_data(self, data):
        """发送数据到串口"""
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.write(data)
                print(f"发送数据: {data.hex() if isinstance(data, bytes) else data}")
                return True
            except Exception as e:
                print(f"发送数据错误: {e}")
                return False
        else:
            print("串口未打开")
            return False
    
    def receive_data(self):
        """从串口接收数据"""
        if self.serial_port and self.serial_port.is_open:
            try:
                # 读取所有可用数据
                data = self.serial_port.read_all()
                if data:
                    print(f"接收数据: {data.hex()}")
                    self.received_data.append(data)
                return data
            except Exception as e:
                print(f"接收数据错误: {e}")
                return None
        else:
            print("串口未打开")
            return None
    
    def continuous_receive(self):
        """持续接收数据的线程函数"""
        while self.is_testing:
            self.receive_data()
            time.sleep(0.1)  # 短暂休眠以减少CPU占用
    
    def simple_loopback_test(self, test_data=b"Hello, ttyS7!"):
        """简单的回环测试"""
        print("\n=== 简单回环测试 ===")
        
        # 清空接收缓冲区
        self.serial_port.reset_input_buffer()
        self.serial_port.reset_output_buffer()
        
        # 发送测试数据
        if not self.send_data(test_data):
            return False
        
        # 等待数据返回
        time.sleep(0.5)
        
        # 接收数据
        received = self.receive_data()
        
        if received == test_data:
            print("✓ 回环测试成功: 发送和接收的数据匹配")
            return True
        else:
            print("✗ 回环测试失败: 发送和接收的数据不匹配")
            print(f"  发送: {test_data}")
            print(f"  接收: {received}")
            return False
    
    def advanced_loopback_test(self, num_packets=5, packet_size=32):
        """高级回环测试 - 发送多个数据包"""
        print(f"\n=== 高级回环测试 ({num_packets}个数据包) ===")
        
        success_count = 0
        
        for i in range(num_packets):
            print(f"\n--- 测试数据包 {i+1}/{num_packets} ---")
            
            # 生成测试数据
            test_data = bytes([(j + i) % 256 for j in range(packet_size)])
            
            # 清空缓冲区
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            
            # 发送数据
            if not self.send_data(test_data):
                continue
            
            # 等待数据返回
            time.sleep(0.5)
            
            # 接收数据
            received = self.receive_data()
            
            if received == test_data:
                print("✓ 数据包测试成功")
                success_count += 1
            else:
                print("✗ 数据包测试失败")
                print(f"  发送: {test_data.hex()[:40]}...")
                print(f"  接收: {received.hex()[:40] if received else '无数据'}...")
        
        success_rate = (success_count / num_packets) * 100
        print(f"\n测试完成: {success_count}/{num_packets} 成功 ({success_rate:.1f}%)")
        
        return success_count == num_packets
    
    def run_interactive_test(self):
        """交互式测试模式"""
        print("\n=== 交互式测试模式 ===")
        print("输入要发送的数据 (输入 'quit' 退出):")
        
        self.is_testing = True
        receive_thread = threading.Thread(target=self.continuous_receive)
        receive_thread.daemon = True
        receive_thread.start()
        
        try:
            while True:
                user_input = input("发送: ")
                if user_input.lower() == 'quit':
                    break
                
                # 发送用户输入的数据
                data_to_send = user_input.encode('utf-8')
                self.send_data(data_to_send)
                
                # 短暂等待接收
                time.sleep(0.5)
                
        except KeyboardInterrupt:
            print("\n测试被用户中断")
        finally:
            self.is_testing = False
    
    def run_comprehensive_test(self):
        """运行全面的回环测试"""
        print("开始串口回环测试")
        print(f"目标串口: {self.port}")
        print(f"波特率: {self.baudrate}")
        
        # 打开串口
        if not self.open_serial():
            return False
        
        try:
            # 运行简单测试
            simple_test_result = self.simple_loopback_test()
            
            # 运行高级测试
            advanced_test_result = self.advanced_loopback_test()
            
            # 总结
            print("\n=== 测试总结 ===")
            if simple_test_result and advanced_test_result:
                print("✓ 所有测试通过! 串口回环功能正常")
                return True
            else:
                print("✗ 部分测试失败! 请检查硬件连接和配置")
                return False
                
        finally:
            self.close_serial()


def main():
    """主函数"""
    # 默认使用 ttyS7，但允许通过命令行参数指定其他串口
    port = 'ttyS7'
    if len(sys.argv) > 1:
        port = sys.argv[1]
    
    # 创建测试实例
    test = SerialLoopbackTest(port=port, baudrate=115200)
    
    print("串口回环测试程序")
    print("1. 运行全面测试")
    print("2. 交互式测试")
    print("3. 简单回环测试")
    print("4. 高级回环测试")
    
    try:
        choice = input("请选择测试模式 (1-4): ").strip()
        
        if choice == '1':
            test.run_comprehensive_test()
        elif choice == '2':
            if test.open_serial():
                test.run_interactive_test()
                test.close_serial()
        elif choice == '3':
            if test.open_serial():
                test.simple_loopback_test()
                test.close_serial()
        elif choice == '4':
            if test.open_serial():
                test.advanced_loopback_test()
                test.close_serial()
        else:
            print("无效选择")
            
    except KeyboardInterrupt:
        print("\n程序被用户中断")
    except Exception as e:
        print(f"程序错误: {e}")


if __name__ == "__main__":
    main()