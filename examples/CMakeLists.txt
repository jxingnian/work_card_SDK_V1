cmake_minimum_required(VERSION 3.14)

project(ehal_sdk_demo C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(camera_web_demo camera_web_demo/camera_web_demo.cpp)
add_executable(wifi_cli_demo wifi_cli_demo/wifi_cli_demo.cpp)

set(EHAL_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(EHAL_SDK_LIB_DIR "${EHAL_SDK_ROOT}/lib")

target_include_directories(camera_web_demo PRIVATE
    "${EHAL_SDK_ROOT}/include"
    "${EHAL_SDK_ROOT}/include/openssl"
)
target_include_directories(wifi_cli_demo PRIVATE
    "${EHAL_SDK_ROOT}/include"
)

target_link_directories(camera_web_demo PRIVATE
    "${EHAL_SDK_LIB_DIR}"
)
target_link_directories(wifi_cli_demo PRIVATE
    "${EHAL_SDK_LIB_DIR}"
)

set(EHAL_CAMERA_CHIP_LINK_LIBS
    ss_mpi
    ss_mpi_sysbind
    ss_mpi_sysmem
    ss_mpi_ae
    ss_mpi_isp
    ot_mpi_isp
    ss_mpi_awb
    dehaze
    extend_stats
    drc
    ldci
    bnr
    calcflicker
    ir_auto
    acs
    securec
    sns_sc2356
    ss_mpi_aibnr
    svp_acl
    ss_mpi_km
    protobuf-c
    ot_osal
    ss_mpi_ive
    ss_mpi_smartae
    ss_mpi_otp
    ss_mpi_cipher
    ss_mpi_uvc
    stdc++
)

target_link_libraries(camera_web_demo PRIVATE
    -Wl,-rpath-link=${EHAL_SDK_LIB_DIR}
    -Wl,--no-as-needed
    ehal_camera
    ehal_audio
    hal
    jsoncpp
    ${EHAL_CAMERA_CHIP_LINK_LIBS}
    ssl
    crypto
    -Wl,--as-needed
    pthread
    dl
    m
)
target_link_options(camera_web_demo PRIVATE
    -Wl,--allow-shlib-undefined
)
target_link_libraries(wifi_cli_demo PRIVATE
    ehal_wifi
    pthread
    dl
    m
)

install(TARGETS camera_web_demo wifi_cli_demo
    RUNTIME DESTINATION bin
)

install(DIRECTORY camera_web_demo/web/
    DESTINATION web
    FILES_MATCHING
        PATTERN "*.html"
        PATTERN "*.css"
        PATTERN "*.js"
)

install(PROGRAMS
    camera_web_demo/run_camera_web_demo.sh
    camera_web_demo/fix_board_sensor_sc2356.sh
    DESTINATION .
)
