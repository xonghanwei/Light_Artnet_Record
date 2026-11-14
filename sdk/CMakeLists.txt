# sdk/CMakeLists.txt — 第三方 SDK DLL (无 Qt Widgets 依赖)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network SerialPort Xml)

# 复用生产源码 (协议 / 输出 / 录制 / 文件格式 / 同步 / 媒体解码)
file(GLOB_RECURSE SDK_CORE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/core/*.h"     "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/network/*.h"  "${CMAKE_SOURCE_DIR}/src/network/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/sync/*.h"     "${CMAKE_SOURCE_DIR}/src/sync/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/protocol/*.h" "${CMAKE_SOURCE_DIR}/src/protocol/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/output/*.h"   "${CMAKE_SOURCE_DIR}/src/output/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/*.h"  "${CMAKE_SOURCE_DIR}/src/storage/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/fileformat/*.h" "${CMAKE_SOURCE_DIR}/src/fileformat/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/recorder/*.h" "${CMAKE_SOURCE_DIR}/src/recorder/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/media/*.h"    "${CMAKE_SOURCE_DIR}/src/media/*.cpp"
)

add_library(artnet_sdk SHARED
    ${SDK_CORE_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ArtNetSdk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ArtNetSdk.h
)
target_compile_definitions(artnet_sdk PRIVATE ARTNET_SDK_BUILD)
target_include_directories(artnet_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/recorder
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/output
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_SOURCE_DIR}/src/sync
    ${CMAKE_SOURCE_DIR}/src/sync/clocks
    ${CMAKE_SOURCE_DIR}/src/fileformat
    ${CMAKE_SOURCE_DIR}/src/media
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(artnet_sdk PRIVATE
    Qt5::Core Qt5::Network Qt5::SerialPort Qt5::Xml
    ${FFMPEG_LIBRARIES}
)
if(WIN32)
    target_link_libraries(artnet_sdk PRIVATE ws2_32 winmm)
endif()

set_target_properties(artnet_sdk PROPERTIES
    PREFIX ""                                   # artnet_sdk.dll
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# FFmpeg 运行库随 DLL 一起放到 bin
file(GLOB FFMPEG_RUNTIME_DLLS
    "${FFMPEG_ROOT}/bin/avcodec-*.dll"
    "${FFMPEG_ROOT}/bin/avformat-*.dll"
    "${FFMPEG_ROOT}/bin/avutil-*.dll"
    "${FFMPEG_ROOT}/bin/swresample-*.dll"
    "${FFMPEG_ROOT}/bin/swscale-*.dll"
    "${FFMPEG_ROOT}/bin/avfilter-*.dll"
    "${FFMPEG_ROOT}/bin/avdevice-*.dll"
)
if(WIN32 AND FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET artnet_sdk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:artnet_sdk>"
        COMMENT "Copying FFmpeg runtime DLLs for SDK"
    )
endif()

# 第三方调用方式演示 + 自动化测试 (纯 Win32 动态加载, 不链接 SDK)
add_executable(artnet_sdk_test test/main.cpp)
set_target_properties(artnet_sdk_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
add_dependencies(artnet_sdk_test artnet_sdk)

# 内建时间码监听验证 (需要 Qt 事件循环, 直连 SDK 导入库)
add_executable(artnet_sdk_tc_test test/tc_listener_test.cpp)
target_include_directories(artnet_sdk_tc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(artnet_sdk_tc_test PRIVATE artnet_sdk Qt5::Core Qt5::Network)
set_target_properties(artnet_sdk_tc_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

message(STATUS "  artnet_sdk — 第三方 C API DLL (artnet_sdk.dll)")
message(STATUS "  artnet_sdk_test — 动态加载测试程序")
message(STATUS "  artnet_sdk_tc_test — 外部时间码监听/同步重演测试")
