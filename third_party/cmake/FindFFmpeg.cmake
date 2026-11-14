# FFmpeg Find Module - 用于查找本地 third_party/ffmpeg 下的 FFmpeg
# 
# 使用方法:
#   在项目根目录创建 third_party/ffmpeg/ 目录, 放置 FFmpeg 库:
#     third_party/ffmpeg/include/   <- 头文件
#     third_party/ffmpeg/lib/       <- 静态库/导入库 (.lib/.a)
#     third_party/ffmpeg/bin/       <- 动态库 (.dll/.so) (可选)
#
# Windows 下推荐使用 BtbN 编译的 FFmpeg: https://github.com/BtbN/FFmpeg-Builds

# 强制 C++17
set(CMAKE_CXX_STANDARD 17)

# 如果用户提供了 FFmpeg 路径
if(NOT FFMPEG_ROOT)
    set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")
endif()

# 检查本地目录
if(EXISTS "${FFMPEG_ROOT}/include" AND EXISTS "${FFMPEG_ROOT}/lib")
    message(STATUS "[FindFFmpeg] 找到本地 FFmpeg: ${FFMPEG_ROOT}")
    
    # 头文件搜索
    find_path(AVFORMAT_INCLUDE_DIR
        NAMES libavformat/avformat.h
        PATHS "${FFMPEG_ROOT}/include"
        NO_DEFAULT_PATH
    )
    
    find_path(AVCODEC_INCLUDE_DIR
        NAMES libavcodec/avcodec.h
        PATHS "${FFMPEG_ROOT}/include"
        NO_DEFAULT_PATH
    )
    
    find_path(AVUTIL_INCLUDE_DIR
        NAMES libavutil/avutil.h
        PATHS "${FFMPEG_ROOT}/include"
        NO_DEFAULT_PATH
    )
    
    find_path(SWRESAMPLE_INCLUDE_DIR
        NAMES libswresample/swresample.h
        PATHS "${FFMPEG_ROOT}/include"
        NO_DEFAULT_PATH
    )
    
    # 库文件搜索
    if(WIN32)
        find_library(AVFORMAT_LIBRARY
            NAMES avformat avformat-61 avformat-60
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
        )
        find_library(AVCODEC_LIBRARY
            NAMES avcodec avcodec-61 avcodec-60
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
        )
        find_library(AVUTIL_LIBRARY
            NAMES avutil avutil-61 avutil-60
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
        )
        find_library(SWRESAMPLE_LIBRARY
            NAMES swresample swresample-6 swresample-5
            PATHS "${FFMPEG_ROOT}/lib"
            NO_DEFAULT_PATH
        )
    else()
        find_library(AVFORMAT_LIBRARY NAMES avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        find_library(AVCODEC_LIBRARY NAMES avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        find_library(AVUTIL_LIBRARY NAMES avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        find_library(SWRESAMPLE_LIBRARY NAMES swresample PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    endif()
    
    set(FFMPEG_INCLUDE_DIRS
        ${AVFORMAT_INCLUDE_DIR}
        ${AVCODEC_INCLUDE_DIR}
        ${AVUTIL_INCLUDE_DIR}
        ${SWRESAMPLE_INCLUDE_DIR}
    )
    
    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
    )
    
    # 检查所有组件是否都找到
    set(FFMPEG_FOUND TRUE)
    foreach(_lib ${FFMPEG_LIBRARIES})
        if(NOT _lib)
            set(FFMPEG_FOUND FALSE)
            break()
        endif()
    endforeach()
    
    if(NOT FFMPEG_FOUND)
        message(WARNING "[FindFFmpeg] 部分 FFmpeg 库未找到, 将禁用媒体解码功能")
    endif()
else()
    message(STATUS "[FindFFmpeg] 未找到本地 FFmpeg, 尝试 pkg-config...")
endif()
