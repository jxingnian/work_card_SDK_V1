#!/usr/bin/env bash

# 任一环境检查、哈希校验或解压失败时立即停止。
set -euo pipefail

# 解析客户 SDK 根目录，允许 SDK 位于任意客户工作目录。
SDK_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# 加载固定工具链名称和路径规则。
# shellcheck source=toolchain_env.sh
source "$SDK_ROOT/scripts/toolchain_env.sh"

# 保存 SDK 内随附的离线工具链包目录。
PACKAGE_ROOT="$SDK_ROOT/toolchain/packages"

# 保存固定工具链归档的完整路径。
ARCHIVE_FILE="$PACKAGE_ROOT/$WORKCARD_TOOLCHAIN_ARCHIVE_NAME"

# 保存固定工具链独立哈希文件路径。
CHECKSUM_FILE="$ARCHIVE_FILE.sha256"

# 要求客户环境提供归档解压命令。
if ! command -v tar >/dev/null 2>&1; then
    echo "开发环境安装失败: 缺少 tar，请先安装 tar" >&2
    exit 127
fi

# 要求客户环境提供标准 SHA-256 校验命令。
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "开发环境安装失败: 缺少 sha256sum，请先安装 coreutils" >&2
    exit 127
fi

# 工具链归档必须随正式 SDK 一起交付。
if [ ! -f "$ARCHIVE_FILE" ]; then
    echo "开发环境安装失败: 缺少 $ARCHIVE_FILE" >&2
    exit 66
fi

# 工具链独立哈希文件必须随正式 SDK 一起交付。
if [ ! -f "$CHECKSUM_FILE" ]; then
    echo "开发环境安装失败: 缺少 $CHECKSUM_FILE" >&2
    exit 66
fi

# 在哈希文件所在目录执行校验，确保其中只使用稳定相对文件名。
(
    # 切换到离线工具链包目录。
    cd "$PACKAGE_ROOT"

    # 在解压之前拒绝任何传输损坏或被替换的工具链包。
    sha256sum -c "$(basename "$CHECKSUM_FILE")"
)

# 默认安装到 WSL 或 Ubuntu 自身的 Linux 文件系统。
DEFAULT_INSTALL_BASE="$(workcard_default_toolchain_base)"

# 允许企业通过环境变量选择另一个 Linux 文件系统安装基目录。
INSTALL_BASE="${WORKCARD_TOOLCHAIN_INSTALL_BASE:-$DEFAULT_INSTALL_BASE}"

# 保存固定版本的父目录。
INSTALL_VERSION_ROOT="$INSTALL_BASE/$WORKCARD_TOOLCHAIN_VERSION_DIR"

# 保存安装完成后的唯一工具链根目录。
INSTALL_TARGET="$INSTALL_VERSION_ROOT/$WORKCARD_TOOLCHAIN_DIR"

# 已正确安装同版本工具链时保持幂等，不重复解压大文件。
if [ -x "$INSTALL_TARGET/bin/arm-linux-musleabi-gcc" ] && \
   [ -x "$INSTALL_TARGET/bin/arm-linux-musleabi-strip" ]; then
    echo "固定工具链已经安装: $INSTALL_TARGET"
    echo "现在可以返回 SDK 根目录执行 eHal Camera SDK 示例编译"
    exit 0
fi

# 发现不完整同名目录时拒绝覆盖，防止误删客户自己的文件。
if [ -e "$INSTALL_TARGET" ]; then
    echo "开发环境安装失败: 已存在不完整目录 $INSTALL_TARGET" >&2
    echo "请人工确认并移走该目录后重新执行安装脚本" >&2
    exit 73
fi

# 创建当前用户专属安装基目录，不要求 sudo 权限。
mkdir -p "$INSTALL_BASE"

# 在目标 Linux 文件系统内创建同盘临时目录，保证最终发布可以原子移动。
TEMP_ROOT="$(mktemp -d "$INSTALL_BASE/.workcard-toolchain-install.XXXXXX")"

# 退出时只清理本脚本创建的明确临时目录。
cleanup_toolchain_temp()
{
    # 临时目录仍存在时删除未完成的解压内容。
    if [ -n "${TEMP_ROOT:-}" ] && [ -d "$TEMP_ROOT" ]; then
        rm -rf -- "$TEMP_ROOT"
    fi
}

# 注册退出清理，避免中断后留下半套工具链。
trap cleanup_toolchain_temp EXIT

# 解压经过 SHA-256 校验的固定工具链包。
tar -xzf "$ARCHIVE_FILE" -C "$TEMP_ROOT"

# 校验归档内固定 C 编译器路径，拒绝错误版本或错误目录结构。
if [ ! -x "$TEMP_ROOT/$WORKCARD_TOOLCHAIN_DIR/bin/arm-linux-musleabi-gcc" ]; then
    echo "开发环境安装失败: 工具链归档目录结构不正确" >&2
    exit 65
fi

# 校验归档内 strip 工具，保证 Demo 构建流程完整。
if [ ! -x "$TEMP_ROOT/$WORKCARD_TOOLCHAIN_DIR/bin/arm-linux-musleabi-strip" ]; then
    echo "开发环境安装失败: 工具链归档缺少 strip" >&2
    exit 65
fi

# 创建固定版本父目录。
mkdir -p "$INSTALL_VERSION_ROOT"

# 将完整工具链一次性发布到最终目录。
mv "$TEMP_ROOT/$WORKCARD_TOOLCHAIN_DIR" "$INSTALL_TARGET"

# 删除已经为空的临时目录。
rmdir "$TEMP_ROOT"

# 清空临时目录变量，防止退出清理误处理已发布目录。
TEMP_ROOT=""

# 输出确定的安装结果和下一步命令。
echo "固定工具链安装完成: $INSTALL_TARGET"
echo "现在可以返回 SDK 根目录执行 eHal Camera SDK 示例编译"
