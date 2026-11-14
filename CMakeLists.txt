cmake_minimum_required(VERSION 3.16)
project(Light_Artnet_Record VERSION 1.2.0 LANGUAGES CXX)

# ============================================================
# C++ 标准 + Qt 自动工具
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 输出目录 (统一)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# 平台检测 + 编译选项
# ============================================================
message(STATUS "")
message(STATUS "=== Platform Detection ===")

if(WIN32)
    message(STATUS "  Target: Windows")
    set(PLATFORM_WINDOWS TRUE)
    if(MSVC)
        add_compile_options(/W3 /MP)
    else()
        # MinGW / Clang-cl on Windows
        add_compile_options(-Wall -Wextra -Wpedantic)
    endif()
    add_definitions(-DUNICODE -D_UNICODE)
elseif(APPLE)
    message(STATUS "  Target: macOS")
    set(PLATFORM_MACOS TRUE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(UNIX)
    message(STATUS "  Target: Linux")
    set(PLATFORM_LINUX TRUE)
    add_compile_options(-Wall -Wextra -Wpedantic)
    find_package(Threads REQUIRED)
endif()

# 通用 Release 优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:   ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "")

# ============================================================
# Qt5 查找 (Core + Network + SerialPort 全平台通用)
# ============================================================
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network SerialPort Xml)

# Linux/macOS 下也需要 Test (测试用)
if(BUILD_TESTING)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test Xml)
endif()

# ============================================================
# FFmpeg 查找 (三策略: third_party/ → pkg-config → 禁用)
# ============================================================
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")
# 兼容随仓库附带的版本化目录 (third_party/ffmpeg-x.y.z)
if(NOT EXISTS "${FFMPEG_ROOT}/include")
    file(GLOB _FFMPEG_CANDIDATES "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-*")
    foreach(_cand ${_FFMPEG_CANDIDATES})
        if(EXISTS "${_cand}/include" AND EXISTS "${_cand}/lib")
            set(FFMPEG_ROOT "${_cand}")
        endif()
    endforeach()
endif()

if(EXISTS "${FFMPEG_ROOT}/include" AND EXISTS "${FFMPEG_ROOT}/lib")
    # 策略 1: 使用 third_party 内置 FFmpeg
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")

    if(WIN32)
        if(MINGW)
            # MinGW 使用 .dll.a 导入库 (同目录下可能同时存在 MSVC 的 .lib)
            file(GLOB FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib/*.dll.a")
            if(NOT FFMPEG_LIBRARIES)
                file(GLOB FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib/*.lib")
            endif()
        else()
            file(GLOB FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib/*.lib")
        endif()
    elseif(APPLE)
        file(GLOB FFMPEG_LIBRARIES
            "${FFMPEG_ROOT}/lib/*.a"
            "${FFMPEG_ROOT}/lib/*.dylib"
        )
    else()
        file(GLOB FFMPEG_LIBRARIES
            "${FFMPEG_ROOT}/lib/*.a"
            "${FFMPEG_ROOT}/lib/*.so*"
        )
    endif()
    add_definitions(-DFFMPEG_AVCODEC_VERSION_CHECK)
    add_definitions(-DARTNET_WITH_FFMPEG)   # 启用 FFmpeg 时钟/解码实现
    message(STATUS "FFmpeg: using bundled ${FFMPEG_ROOT}")

else()
    # 策略 2: 系统 pkg-config (Linux/macOS 首选)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVFORMAT libavformat)
        pkg_check_modules(AVCODEC  libavcodec)
        pkg_check_modules(AVUTIL   libavutil)
        pkg_check_modules(SWRESAMPLE libswresample)

        if(AVFORMAT_FOUND AND AVCODEC_FOUND AND AVUTIL_FOUND AND SWRESAMPLE_FOUND)
            set(FFMPEG_INCLUDE_DIRS
                ${AVFORMAT_INCLUDE_DIRS}
                ${AVCODEC_INCLUDE_DIRS}
                ${AVUTIL_INCLUDE_DIRS}
                ${SWRESAMPLE_INCLUDE_DIRS}
            )
            set(FFMPEG_LIBRARIES
                ${AVFORMAT_LIBRARIES}
                ${AVCODEC_LIBRARIES}
                ${AVUTIL_LIBRARIES}
                ${SWRESAMPLE_LIBRARIES}
            )
            add_definitions(-DFFMPEG_AVCODEC_VERSION_CHECK)
            add_definitions(-DARTNET_WITH_FFMPEG)
            message(STATUS "FFmpeg: found via pkg-config")
        else()
            message(WARNING "FFmpeg not found. Media features will be disabled.")
            set(FFMPEG_INCLUDE_DIRS "")
            set(FFMPEG_LIBRARIES "")
            add_definitions(-DNO_FFMPEG)
        endif()
    else()
        message(WARNING "PkgConfig not found. Media features disabled.")
        set(FFMPEG_INCLUDE_DIRS "")
        set(FFMPEG_LIBRARIES "")
        add_definitions(-DNO_FFMPEG)
    endif()
endif()

message(STATUS "FFmpeg Include: ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "FFmpeg Libs:     ${FFMPEG_LIBRARIES}")

# ============================================================
# 添加子目录
# ============================================================
add_subdirectory(src)

option(BUILD_TESTING "Build unit and integration tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
# 第三方 SDK (无界面 C API DLL: artnet_sdk.dll)
# ============================================================
option(BUILD_SDK "Build third-party SDK DLL" ON)
if(BUILD_SDK)
    add_subdirectory(sdk)
endif()

# ============================================================
# 安装规则 (跨平台)
# ============================================================
install(TARGETS artnet_recorder
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# FFmpeg DLL 一并安装 (Windows)
if(WIN32 AND FFMPEG_LIBRARIES)
    install(DIRECTORY "${FFMPEG_ROOT}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
endif()

# ============================================================
# CPack 打包 (可选, 构建时 -DCPACK_GENERATOR=ZIP)
# ============================================================
option(ENABLE_CPACK "Enable CPack packaging" OFF)
if(ENABLE_CPACK)
    include(CPack)
    set(CPACK_PACKAGE_NAME "Light_Artnet_Record")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ArtNet/DMX512/NPU protocol recorder")
    set(CPACK_PACKAGE_CONTACT "developer@example.com")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

    if(WIN32)
        set(CPACK_GENERATOR "ZIP;NSIS")
    elseif(APPLE)
        set(CPACK_GENERATOR "TGZ;dragdrop")
    else()
        set(CPACK_GENERATOR "TGZ;DEB;RPM")
    endif()
endif()

# ============================================================
# 最终状态
# ============================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  Light_Artnet_Record CMake Configuration")
message(STATUS "========================================")
message(STATUS "  C++ Standard:   C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests:    ${BUILD_TESTING}")
message(STATUS "  FFmpeg Found:   ${FFMPEG_INCLUDE_DIRS}")
message(STATUS "========================================")
message(STATUS "")
