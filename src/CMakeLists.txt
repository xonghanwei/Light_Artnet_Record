# src CMakeLists.txt

# 收集源文件
file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
)

file(GLOB_RECURSE NETWORK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/network/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/network/*.cpp"
)

file(GLOB_RECURSE MEDIA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/media/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/media/*.cpp"
)

file(GLOB_RECURSE RECORDER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/recorder/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/recorder/*.cpp"
)

# 存储抽象层 + 所有实现 (ArtRec / JSON / XML)
file(GLOB_RECURSE STORAGE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
)

# 原有 ArtRec 二进制格式 (storage/ArtRecStorage 内部组合使用)
file(GLOB_RECURSE FILEFORMAT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/fileformat/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/fileformat/*.cpp"
)

# Protocol 层
file(GLOB_RECURSE PROTOCOL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/sources/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/sources/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/sinks/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/protocol/sinks/*.cpp"
)

file(GLOB_RECURSE OUTPUT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/output/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/output/*.cpp"
)

# 同步抽象层 + 所有时钟实现
file(GLOB_RECURSE SYNC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/sync/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/sync/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sync/clocks/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/sync/clocks/*.cpp"
)

# 主程序源文件
file(GLOB MAIN_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
)

# ============================================================
# 主可执行文件
# ============================================================
add_executable(artnet_recorder
    ${MAIN_SOURCES}
    ${CORE_SOURCES}
    ${NETWORK_SOURCES}
    ${MEDIA_SOURCES}
    ${RECORDER_SOURCES}
    ${STORAGE_SOURCES}
    ${FILEFORMAT_SOURCES}
    ${OUTPUT_SOURCES}
    ${PROTOCOL_SOURCES}
    ${SYNC_SOURCES}
)

target_include_directories(artnet_recorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_SOURCE_DIR}/media
    ${CMAKE_CURRENT_SOURCE_DIR}/recorder
    ${CMAKE_CURRENT_SOURCE_DIR}/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/output
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/sync
    ${CMAKE_CURRENT_SOURCE_DIR}/sync/clocks
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(artnet_recorder PRIVATE
    Qt5::Core
    Qt5::Network
    Qt5::SerialPort
    Qt5::Xml
    ${FFMPEG_LIBRARIES}
)

# ============================================================
# 跨平台额外链接
# ============================================================
if(WIN32)
    # ws2_32: Winsock / winmm: MIDI 输入 (MidiMTCClock)
    target_link_libraries(artnet_recorder PRIVATE ws2_32 winmm)
elseif(APPLE)
    # macOS: 无需额外, Qt 已封装
elseif(UNIX)
    target_link_libraries(artnet_recorder PRIVATE Threads::Threads)
endif()
