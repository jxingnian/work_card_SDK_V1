cmake_minimum_required(VERSION 3.14)

project(camera_web_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EHAL_CAMERA_SDK_DIR "" CACHE PATH
    "Path to the installed ehal camera SDK")
if(EHAL_CAMERA_SDK_DIR STREQUAL "")
    message(FATAL_ERROR
        "Set EHAL_CAMERA_SDK_DIR to the SDK installation directory")
endif()

find_path(EHAL_CAMERA_INCLUDE_DIR
    NAMES ehal_camera.h
    PATHS "${EHAL_CAMERA_SDK_DIR}/include"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
find_library(EHAL_CAMERA_LIBRARY
    NAMES ehal_camera
    PATHS "${EHAL_CAMERA_SDK_DIR}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
find_library(EHAL_AUDIO_LIBRARY
    NAMES ehal_audio
    PATHS "${EHAL_CAMERA_SDK_DIR}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
if(NOT EHAL_CAMERA_INCLUDE_DIR OR NOT EHAL_CAMERA_LIBRARY OR NOT EHAL_AUDIO_LIBRARY)
    message(FATAL_ERROR
        "ehal camera/audio headers or libraries were not found in EHAL_CAMERA_SDK_DIR")
endif()

set(EHAL_CAMERA_DEVENV_DIR "${EHAL_CAMERA_SDK_DIR}/docs/开发环境")
set(EHAL_CAMERA_RUNTIME_INCLUDE_DIR "${EHAL_CAMERA_DEVENV_DIR}/runtime/include")
set(EHAL_CAMERA_RUNTIME_LIBRARY_DIR "${EHAL_CAMERA_DEVENV_DIR}/runtime/lib")

add_executable(camera_web_demo camera_web_demo.cpp)

target_include_directories(camera_web_demo PRIVATE
    "${EHAL_CAMERA_INCLUDE_DIR}"
    "${EHAL_CAMERA_RUNTIME_INCLUDE_DIR}"
)

target_link_directories(camera_web_demo PRIVATE
    "${EHAL_CAMERA_RUNTIME_LIBRARY_DIR}"
)

target_link_libraries(camera_web_demo PRIVATE
    "${EHAL_CAMERA_LIBRARY}"
    "${EHAL_AUDIO_LIBRARY}"
    crypto
    pthread
    dl
    m
)
target_link_options(camera_web_demo PRIVATE
    -Wl,--allow-shlib-undefined
)

install(TARGETS camera_web_demo
    RUNTIME DESTINATION bin
)

install(DIRECTORY web/
    DESTINATION web
    FILES_MATCHING
        PATTERN "*.html"
        PATTERN "*.css"
        PATTERN "*.js"
)

install(PROGRAMS
    run_camera_web_demo.sh
    fix_board_sensor_sc2356.sh
    DESTINATION .
)
