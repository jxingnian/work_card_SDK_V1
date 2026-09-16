#!/usr/bin/env bash

# 本文件只定义固定工具链版本和定位函数，由安装脚本与编译脚本共同复用。

# 固定客户工具链的版本目录名称。
WORKCARD_TOOLCHAIN_VERSION_DIR="gcc-20250305-arm-v01c02-linux-musleabi"

# 固定客户工具链的实际根目录名称。
WORKCARD_TOOLCHAIN_DIR="arm-v01c02-linux-musleabi-gcc"

# 固定工具链归档文件名称，避免安装脚本使用模糊匹配。
WORKCARD_TOOLCHAIN_ARCHIVE_NAME="workcard-arm-linux-musleabi-toolchain-20250305.tar.gz"

# 返回当前用户默认的工具链安装基目录。
workcard_default_toolchain_base()
{
    # WSL 和原生 Ubuntu 都必须提供 HOME，避免把工具链写入未知目录。
    if [ -z "${HOME:-}" ]; then
        echo "工具链定位失败: 当前 Linux 环境没有设置 HOME" >&2
        return 64
    fi

    # 默认安装在 Linux 文件系统中，避免 Windows NTFS 损坏符号链接和执行权限。
    printf '%s\n' "$HOME/.local/workcard-toolchain"
}

# 根据显式配置或默认位置解析可用工具链。
workcard_resolve_toolchain()
{
    # 保存调用方通过 TOOLCHAIN_ROOT 或 WORKCARD_TOOLCHAIN_ROOT 指定的目录。
    local requested_root="${1:-}"

    # 显式路径优先，便于企业统一安装到受管目录。
    if [ -n "$requested_root" ]; then
        WORKCARD_RESOLVED_TOOLCHAIN_ROOT="$requested_root"
    else
        # 解析当前用户的默认安装基目录。
        local default_base
        default_base="$(workcard_default_toolchain_base)" || return $?

        # 拼出 setup_dev_env.sh 生成的固定版本目录。
        WORKCARD_RESOLVED_TOOLCHAIN_ROOT="$default_base/$WORKCARD_TOOLCHAIN_VERSION_DIR/$WORKCARD_TOOLCHAIN_DIR"
    fi

    # 固定检查真实 C 编译器，不能只凭目录存在判断安装成功。
    if [ ! -x "$WORKCARD_RESOLVED_TOOLCHAIN_ROOT/bin/arm-linux-musleabi-gcc" ]; then
        echo "工具链定位失败: 缺少 $WORKCARD_RESOLVED_TOOLCHAIN_ROOT/bin/arm-linux-musleabi-gcc" >&2
        echo "请在 SDK 根目录执行: ./scripts/setup_dev_env.sh" >&2
        return 127
    fi

    # 固定检查 strip 工具，避免编译完成后在交付阶段才失败。
    if [ ! -x "$WORKCARD_RESOLVED_TOOLCHAIN_ROOT/bin/arm-linux-musleabi-strip" ]; then
        echo "工具链定位失败: 缺少 $WORKCARD_RESOLVED_TOOLCHAIN_ROOT/bin/arm-linux-musleabi-strip" >&2
        echo "请重新安装本 SDK 随附的固定工具链" >&2
        return 127
    fi

    # 导出解析结果，供后续子进程和 CMake 复用。
    export WORKCARD_RESOLVED_TOOLCHAIN_ROOT
}
