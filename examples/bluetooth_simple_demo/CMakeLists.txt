cmake_minimum_required(VERSION 3.10)
project(bluetooth_simple_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含SDK头文件
include_directories(${CMAKE_SOURCE_DIR}/../../include)

# 链接SDK库目录
link_directories(${CMAKE_SOURCE_DIR}/../../lib)

# 添加可执行文件
add_executable(bluetooth_simple_demo
    bluetooth_simple_demo.cpp
)

# 链接库
target_link_libraries(bluetooth_simple_demo
    ehal_bluetooth
    pthread
)

# 安装规则
install(TARGETS bluetooth_simple_demo
    RUNTIME DESTINATION bin
)

# 复制web文件
install(DIRECTORY web
    DESTINATION bin
)
