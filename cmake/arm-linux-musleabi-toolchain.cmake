# 固定目标系统为 Linux，禁止 CMake 使用宿主编译器。
set(CMAKE_SYSTEM_NAME Linux)

# 固定目标处理器为三十二位 ARM。
set(CMAKE_SYSTEM_PROCESSOR arm)

# 显式环境变量存在时优先使用企业指定的固定工具链目录。
if(DEFINED ENV{WORKCARD_TOOLCHAIN_ROOT} AND NOT "$ENV{WORKCARD_TOOLCHAIN_ROOT}" STREQUAL "")
    set(WORKCARD_TOOLCHAIN_ROOT "$ENV{WORKCARD_TOOLCHAIN_ROOT}")
else()
    # 默认使用 setup_dev_env.sh 在当前 Linux 用户目录安装的位置。
    set(WORKCARD_TOOLCHAIN_ROOT "$ENV{HOME}/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc")
endif()

# 在 CMake 配置阶段给出确定的缺失路径和安装命令。
if(NOT EXISTS "${WORKCARD_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-gcc")
    message(FATAL_ERROR "找不到固定工具链: ${WORKCARD_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-gcc\n请先执行 docs/开发环境/scripts/setup_dev_env.sh")
endif()

# 固定 C 编译器路径。
set(CMAKE_C_COMPILER "${WORKCARD_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-gcc")

# 固定 C++ 编译器路径。
set(CMAKE_CXX_COMPILER "${WORKCARD_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-g++")

# 固定 strip 工具路径。
set(CMAKE_STRIP "${WORKCARD_TOOLCHAIN_ROOT}/bin/arm-linux-musleabi-strip")

# 禁止 CMake 在宿主系统目录中误找目标库。
set(CMAKE_FIND_ROOT_PATH "${WORKCARD_TOOLCHAIN_ROOT}/target")

# 程序只在宿主环境中查找。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# 库和头文件优先在目标工具链中查找。
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
