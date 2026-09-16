cmake_minimum_required(VERSION 3.14)

project(ehal_sdk_demo C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(camera_demo camera_demo.c)
add_executable(wifi_demo wifi_demo.c)
add_executable(wifi_web_demo wifi_web_demo/wifi_web_demo.cpp)

target_include_directories(camera_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)
target_include_directories(wifi_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)
target_include_directories(wifi_web_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)

target_link_directories(camera_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../lib"
)
target_link_directories(wifi_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../lib"
)
target_link_directories(wifi_web_demo PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../lib"
)

target_link_libraries(camera_demo PRIVATE
    ehal_camera
    pthread
    dl
    m
)
target_link_libraries(wifi_demo PRIVATE
    ehal_wifi
    stdc++
    pthread
    dl
    m
)
set_target_properties(wifi_demo PROPERTIES
    LINKER_LANGUAGE CXX
)
target_link_libraries(wifi_web_demo PRIVATE
    ehal_wifi
    pthread
    dl
    m
)
