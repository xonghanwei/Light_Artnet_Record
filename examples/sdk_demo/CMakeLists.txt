# examples/sdk_demo/CMakeLists.txt — 第三方 SDK 示例 (独立工程, 不参与主工程构建)
#
# 前提: 已构建 SDK
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build --target artnet_sdk
#
# 构建示例:
#   cmake -S examples/sdk_demo -B build/example_demo -G "MinGW Makefiles"
#   cmake --build build/example_demo
#
# 若把示例拷到别的目录, 用 -DARTNET_SDK_ROOT=<Light_Artnet_Record 根目录> 指定.
cmake_minimum_required(VERSION 3.16)
project(ArtNetSdkDemo LANGUAGES C)

set(ARTNET_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Light_Artnet_Record 根目录")
set(ARTNET_SDK_BIN "${ARTNET_SDK_ROOT}/build/bin")

# 导入库: MinGW 为 libartnet_sdk.dll.a, MSVC 为 artnet_sdk.lib
find_library(ARTNET_SDK_IMPLIB
    NAMES artnet_sdk
    PATHS "${ARTNET_SDK_BIN}" "${ARTNET_SDK_ROOT}/build/lib"
    NO_DEFAULT_PATH
)
if(NOT ARTNET_SDK_IMPLIB)
    message(FATAL_ERROR
        "未找到 artnet_sdk 导入库. 请先构建 SDK, 或 -DARTNET_SDK_IMPLIB=<完整路径>")
endif()
if(NOT EXISTS "${ARTNET_SDK_BIN}/artnet_sdk.dll")
    message(FATAL_ERROR "未找到 artnet_sdk.dll: ${ARTNET_SDK_BIN}")
endif()

add_executable(ArtNetSdkDemo main.c)
target_include_directories(ArtNetSdkDemo PRIVATE "${ARTNET_SDK_ROOT}/sdk/include")
target_link_libraries(ArtNetSdkDemo PRIVATE "${ARTNET_SDK_IMPLIB}")

# 运行时把 DLL 放到 exe 同级
add_custom_command(TARGET ArtNetSdkDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ARTNET_SDK_BIN}/artnet_sdk.dll" "$<TARGET_FILE_DIR:ArtNetSdkDemo>"
    COMMENT "Copying artnet_sdk.dll")

# SDK 依赖的 FFmpeg 运行库 (含 avcodec 的传递依赖) 一并复制
file(GLOB SDKO_DEP_DLLS
    "${ARTNET_SDK_BIN}/avcodec-*.dll"
    "${ARTNET_SDK_BIN}/avformat-*.dll"
    "${ARTNET_SDK_BIN}/avutil-*.dll"
    "${ARTNET_SDK_BIN}/avfilter-*.dll"
    "${ARTNET_SDK_BIN}/avdevice-*.dll"
    "${ARTNET_SDK_BIN}/swresample-*.dll"
    "${ARTNET_SDK_BIN}/swscale-*.dll"
)
if(SDKO_DEP_DLLS)
    add_custom_command(TARGET ArtNetSdkDemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SDKO_DEP_DLLS} "$<TARGET_FILE_DIR:ArtNetSdkDemo>"
        COMMENT "Copying FFmpeg runtime DLLs")
endif()

# 说明: Qt5 DLL (Qt5Core/Network/SerialPort/Xml) 需在 PATH 中,
#       或对 artnet_sdk.dll 执行 windeployqt 后与程序一起分发.

message(STATUS "SDK 头文件: ${ARTNET_SDK_ROOT}/sdk/include")
message(STATUS "SDK 导入库: ${ARTNET_SDK_IMPLIB}")
message(STATUS "SDK 运行库: ${ARTNET_SDK_BIN}/artnet_sdk.dll")
