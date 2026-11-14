# tests/ui/CMakeLists.txt — 功能测试 UI 构建
# 独立可执行程序, 只在手动"功能测试"时运行, 不纳入 ctest
# 使用 Qt5::Widgets, 复用生产源码 (core/network/sync 等)

find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

# ===== 复用生产源码 =====
# 与 src/CMakeLists.txt 保持同步 — 除 main.cpp 外全部源文件
file(GLOB_RECURSE UI_CORE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/core/*.h"
    "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
)
file(GLOB_RECURSE UI_NETWORK_SOURCES
    "${CMAKE_SOURCE_DIR}/src/network/*.h"
    "${CMAKE_SOURCE_DIR}/src/network/*.cpp"
)
file(GLOB_RECURSE UI_SYNC_SOURCES
    "${CMAKE_SOURCE_DIR}/src/sync/*.h"
    "${CMAKE_SOURCE_DIR}/src/sync/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/sync/clocks/*.h"
    "${CMAKE_SOURCE_DIR}/src/sync/clocks/*.cpp"
)
file(GLOB_RECURSE UI_PROTOCOL_SOURCES
    "${CMAKE_SOURCE_DIR}/src/protocol/*.h"
    "${CMAKE_SOURCE_DIR}/src/protocol/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/protocol/sources/*.h"
    "${CMAKE_SOURCE_DIR}/src/protocol/sources/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/protocol/sinks/*.h"
    "${CMAKE_SOURCE_DIR}/src/protocol/sinks/*.cpp"
)
file(GLOB_RECURSE UI_OUTPUT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/output/*.h"
    "${CMAKE_SOURCE_DIR}/src/output/*.cpp"
)
file(GLOB_RECURSE UI_STORAGE_SOURCES
    "${CMAKE_SOURCE_DIR}/src/storage/*.h"
    "${CMAKE_SOURCE_DIR}/src/storage/*.cpp"
)
file(GLOB_RECURSE UI_FILEFORMAT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/fileformat/*.h"
    "${CMAKE_SOURCE_DIR}/src/fileformat/*.cpp"
)
file(GLOB_RECURSE UI_RECORDER_SOURCES
    "${CMAKE_SOURCE_DIR}/src/recorder/*.h"
    "${CMAKE_SOURCE_DIR}/src/recorder/*.cpp"
)
file(GLOB UI_MEDIA_SOURCES
    "${CMAKE_SOURCE_DIR}/src/media/*.h"
    "${CMAKE_SOURCE_DIR}/src/media/*.cpp"
)

# ===== 基类 =====
file(GLOB UI_BASE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cpp"
)

# ===== 功能测试面板 (SCADA) — 自动收集 demos/ 所有源文件 =====
file(GLOB UI_DEMO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/demos/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/demos/*.cpp"
)
# 插件/可执行程序共用的源文件 (排除 main.cpp, 否则 DLL 里会多一份 main)
list(FILTER UI_DEMO_SOURCES EXCLUDE REGEX "/main\\.cpp$")

set(UI_PLUGIN_SOURCES
    ${UI_BASE_SOURCES}
    ${UI_DEMO_SOURCES}
    ${UI_CORE_SOURCES}
    ${UI_NETWORK_SOURCES}
    ${UI_SYNC_SOURCES}
    ${UI_PROTOCOL_SOURCES}
    ${UI_OUTPUT_SOURCES}
    ${UI_STORAGE_SOURCES}
    ${UI_FILEFORMAT_SOURCES}
    ${UI_RECORDER_SOURCES}
    ${UI_MEDIA_SOURCES}
)

set(UI_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/recorder
    ${CMAKE_SOURCE_DIR}/src/storage
    ${CMAKE_SOURCE_DIR}/src/output
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_SOURCE_DIR}/src/sync
    ${CMAKE_SOURCE_DIR}/src/sync/clocks
    ${CMAKE_SOURCE_DIR}/src/media
    ${CMAKE_SOURCE_DIR}/src/fileformat
    ${CMAKE_CURRENT_SOURCE_DIR}/base
    ${FFMPEG_INCLUDE_DIRS}
)

set(UI_LINK_LIBS
    Qt5::Widgets
    Qt5::Core
    Qt5::Network
    Qt5::SerialPort
    Qt5::Xml
    Qt5::Multimedia
    Qt5::MultimediaWidgets
    ${FFMPEG_LIBRARIES}
)

add_executable(artnet_recorder_ui WIN32
    ${UI_PLUGIN_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/demos/main.cpp
)

target_include_directories(artnet_recorder_ui PRIVATE ${UI_INCLUDE_DIRS})

target_link_libraries(artnet_recorder_ui PRIVATE ${UI_LINK_LIBS})

if(WIN32)
    target_link_libraries(artnet_recorder_ui PRIVATE ws2_32 dwmapi winmm)
endif()

# ===== UI 插件 DLL (供宿主程序加载) =====
add_library(artnet_ui_plugin SHARED
    ${UI_PLUGIN_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/ArtNetUiPlugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/ArtNetUiPlugin.h
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/ArtNetPlaybackApi.cpp
)
target_compile_definitions(artnet_ui_plugin PRIVATE ARTNET_UI_PLUGIN_BUILD)
target_include_directories(artnet_ui_plugin PRIVATE ${UI_INCLUDE_DIRS})
target_link_libraries(artnet_ui_plugin PRIVATE ${UI_LINK_LIBS})
if(WIN32)
    target_link_libraries(artnet_ui_plugin PRIVATE ws2_32 dwmapi winmm)
endif()
set_target_properties(artnet_ui_plugin PROPERTIES
    PREFIX ""                                  # 生成 artnet_ui_plugin.dll (去掉 MinGW 的 lib 前缀)
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# ===== 插件测试台 (Qt 图形程序, 加载 DLL 并验证) =====
add_subdirectory(plugin_test)

# ===== 复制 QSS 皮肤到可执行文件同级 skins/ 目录 =====
# 用独立目标而不是 POST_BUILD: 只改 .qss 时不会触发重新链接,
# POST_BUILD 就不会执行, 导致 exe 仍加载旧皮肤.
set(UI_SKIN_OUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/skins/dark_skin.qss")
add_custom_command(OUTPUT "${UI_SKIN_OUT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/skins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/base/dark_skin.qss"
        "${UI_SKIN_OUT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/base/dark_skin.qss"
    COMMENT "Copying dark skin QSS to build output"
)
add_custom_target(copy_ui_skins ALL DEPENDS "${UI_SKIN_OUT}")
# 让主 target 也带上皮肤复制, 否则单构建 artnet_recorder_ui 时不会更新 skins/
add_dependencies(artnet_recorder_ui copy_ui_skins)
add_dependencies(artnet_ui_plugin copy_ui_skins)

# ===== 复制 FFmpeg 运行库到可执行文件同级目录 (Windows) =====
if(WIN32 AND FFMPEG_LIBRARIES AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_RUNTIME_DLLS
        "${FFMPEG_ROOT}/bin/avcodec-*.dll"
        "${FFMPEG_ROOT}/bin/avformat-*.dll"
        "${FFMPEG_ROOT}/bin/avutil-*.dll"
        "${FFMPEG_ROOT}/bin/swresample-*.dll"
        "${FFMPEG_ROOT}/bin/swscale-*.dll"
        "${FFMPEG_ROOT}/bin/avfilter-*.dll"
        "${FFMPEG_ROOT}/bin/avdevice-*.dll"
    )
    if(FFMPEG_RUNTIME_DLLS)
        foreach(_ui_tgt artnet_recorder_ui artnet_ui_plugin)
            add_custom_command(TARGET ${_ui_tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${_ui_tgt}>"
                COMMENT "Copying FFmpeg runtime DLLs"
            )
        endforeach()
    endif()
endif()

# ===== 不纳入 ctest — 这是手动运行的功能测试 UI =====
message(STATUS "  artnet_recorder_ui — 功能测试 UI (手动运行)")
