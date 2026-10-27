cmake_minimum_required(VERSION 3.10)
project(bluetooth_simple_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件 - 使用底层实现版本
add_executable(bluetooth_simple_demo
    bluetooth_simple_demo.cpp
)

# 只链接 pthread，不需要 ehal_bluetooth
target_link_libraries(bluetooth_simple_demo
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
