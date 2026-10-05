#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
编译并部署 location_cli_demo 到核心板
"""
import paramiko
import os
import sys
import subprocess

BOARD_IP = "192.168.137.120"
BOARD_USER = "root"
BOARD_PASS = "ebaina"

PROJECT_ROOT = r"D:\XingNian\client\Steve669063\ehal_media-master\work_card_SDK_V1"
BUILD_DIR = os.path.join(PROJECT_ROOT, "build", "location_cli_demo")
TOOLCHAIN = os.path.join(PROJECT_ROOT, "cmake", "arm-linux-musleabi-toolchain.cmake")
EXAMPLES_DIR = os.path.join(PROJECT_ROOT, "examples")

def build():
    print("=" * 70)
    print("开始编译 location_cli_demo")
    print("=" * 70)

    # 创建独立 build 目录
    os.makedirs(BUILD_DIR, exist_ok=True)

    # CMake 配置
    cmake_cmd = f'cmake -DCMAKE_TOOLCHAIN_FILE="{TOOLCHAIN}" "{EXAMPLES_DIR}"'
    print(f"运行: {cmake_cmd}")
    print(f"工作目录: {BUILD_DIR}")

    result = subprocess.run(cmake_cmd, shell=True, cwd=BUILD_DIR)
    if result.returncode != 0:
        print("CMake 配置失败")
        return False

    # 只编译 location_cli_demo
    build_cmd = "cmake --build . --target location_cli_demo"
    print(f"\n运行: {build_cmd}")

    result = subprocess.run(build_cmd, shell=True, cwd=BUILD_DIR)
    if result.returncode != 0:
        print("编译失败")
        return False

    print("\n编译成功!")
    return True

def deploy():
    print("\n" + "=" * 70)
    print("开始部署到核心板")
    print("=" * 70)

    binary = os.path.join(BUILD_DIR, "location_cli_demo")
    if not os.path.exists(binary):
        print(f"错误: 找不到 {binary}")
        return False

    try:
        print(f"连接 {BOARD_IP}...")
        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(BOARD_IP, username=BOARD_USER, password=BOARD_PASS, timeout=10)

        print("上传 location_cli_demo...")
        sftp = ssh.open_sftp()
        sftp.put(binary, "/tmp/location_cli_demo")
        sftp.close()

        ssh.exec_command("chmod +x /tmp/location_cli_demo")

        print("\n" + "=" * 70)
        print("运行测试")
        print("=" * 70)

        stdin, stdout, stderr = ssh.exec_command("/tmp/location_cli_demo", timeout=10)

        output = stdout.read().decode('utf-8', errors='ignore')
        error = stderr.read().decode('utf-8', errors='ignore')

        if output:
            print(output)
        if error:
            print(error)

        ssh.close()

        print("\n" + "=" * 70)
        print("部署成功!")
        print("\n在核心板上运行: /tmp/location_cli_demo")
        print("=" * 70)

        return True

    except Exception as e:
        print(f"部署失败: {e}", file=sys.stderr)
        return False

if __name__ == '__main__':
    if not build():
        sys.exit(1)
    if not deploy():
        sys.exit(1)
