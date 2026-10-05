#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
通过 SSH 在核心板上编译并测试 location_cli_demo
前提：核心板上需要有交叉编译工具链
"""
import paramiko
import sys

BOARD_IP = "192.168.137.120"
BOARD_USER = "root"
BOARD_PASS = "ebaina"

def main():
    print("=" * 70)
    print("通过 SSH 在核心板上编译 location_cli_demo")
    print("=" * 70)

    try:
        print(f"\n连接核心板 {BOARD_IP}...")
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(BOARD_IP, username=BOARD_USER, password=BOARD_PASS, timeout=10)

        # 直接在核心板上使用已编译的二进制或通过其他方式获取
        # 由于核心板上也没有编译工具，我们直接测试 HTTP 获取定位

        print("\n" + "=" * 70)
        print("测试定位 HTTP 接口")
        print("=" * 70)

        # 通过 SSH 隧道获取定位数据
        channel = ssh.get_transport().open_channel(
            'direct-tcpip',
            ('192.168.5.1', 8080),
            ('127.0.0.1', 0)
        )

        request = b'GET /location HTTP/1.1\r\nHost: 192.168.5.1\r\nConnection: close\r\n\r\n'
        channel.sendall(request)

        response = b''
        while True:
            try:
                chunk = channel.recv(4096)
                if not chunk:
                    break
                response += chunk
            except:
                break

        channel.close()
        ssh.close()

        response_str = response.decode('utf-8', errors='ignore')

        if '\r\n\r\n' in response_str:
            parts = response_str.split('\r\n\r\n', 1)
            if len(parts) == 2:
                json_data = parts[1].strip()

                print("\n定位数据:")
                print(json_data)
                print("\n" + "=" * 70)
                print("说明:")
                print("- 核心板通过 HTTP 接口访问: http://192.168.5.1:8080/location")
                print("- 你可以用已编译的程序或脚本从核心板调用此接口")
                print("=" * 70)

                return True

        print("获取定位数据失败")
        return False

    except Exception as e:
        print(f"错误: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return False

if __name__ == '__main__':
    sys.exit(0 if main() else 1)
