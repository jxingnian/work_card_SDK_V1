cmake_minimum_required(VERSION 3.14)

project(hardware_test_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仅链接本 Demo 所需的 SDK 和厂家媒体库，不引入其他示例程序。
set(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SDK_INCLUDE_DIR "${SDK_ROOT}/include")
set(SDK_LIB_DIR "${SDK_ROOT}/lib")

add_executable(hardware_test_demo
    src/main.cpp
    src/http_server.cpp
    src/wifi_native.cpp
    src/native_command.cpp
    src/camera_native.cpp
    src/audio_native.cpp
    src/bluetooth_native.cpp
    src/network_native.cpp
    src/key_native.cpp
    src/led_native.cpp
)

target_include_directories(hardware_test_demo PRIVATE
    "${SDK_INCLUDE_DIR}"
    "${SDK_INCLUDE_DIR}/chip/hi3516cv610"
    src
)

target_link_directories(hardware_test_demo PRIVATE "${SDK_LIB_DIR}")

target_link_libraries(hardware_test_demo PRIVATE
    -Wl,--no-as-needed
    ehal_camera ehal_audio ehal_bluetooth ehal_key hal jsoncpp
    ss_mpi ss_mpi_sysbind ss_mpi_sysmem ss_mpi_ae ss_mpi_isp ot_mpi_isp
    ss_mpi_awb dehaze extend_stats drc ldci bnr calcflicker ir_auto acs securec
    sns_fpx1002 ss_mpi_aibnr svp_acl ss_mpi_km protobuf-c ot_osal ss_mpi_ive
    ss_mpi_smartae ss_mpi_otp ss_mpi_cipher ss_mpi_uvc stdc++
    ss_mpi_audio ss_mpi_audio_adp voice_engine upvqe dnvqe vqe_res vqe_record
    vqe_hpf vqe_anr vqe_aec vqe_agc vqe_eq vqe_talkv2 opus aac_dec
    -Wl,--as-needed pthread dl m
)

target_link_options(hardware_test_demo PRIVATE
    -Wl,-rpath-link=${SDK_LIB_DIR}
    -Wl,--allow-shlib-undefined
)
