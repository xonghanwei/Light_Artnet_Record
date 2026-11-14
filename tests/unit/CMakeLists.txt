# tests/unit/CMakeLists.txt
#
# 单元测试: 每个 test 一个可执行文件, 自动注册到 ctest
# 设计原则:
#   - 只测试公开接口, 不 include 私有 .cpp
#   - 每个测试目标自包含, 不互相依赖
#   - 运行命令: ctest -C Release --output-on-failure
#

find_package(Qt5 REQUIRED COMPONENTS Test Core Network SerialPort Xml)

# 公共包含目录: src 的公开头文件
set(ARTNET_SRC_ROOT ${CMAKE_SOURCE_DIR}/src)

# 收集各模块需要的 .cpp 实现文件 (测试需要链接这些)
set(CORE_SOURCES
    ${ARTNET_SRC_ROOT}/core/Logger.cpp
)

set(NETWORK_SOURCES
    ${ARTNET_SRC_ROOT}/network/ArtNetReceiver.cpp
)

set(OUTPUT_SOURCES
    ${ARTNET_SRC_ROOT}/output/ArtNetSender.cpp
    ${ARTNET_SRC_ROOT}/output/Dmx512Sender.cpp
)

set(FILEFORMAT_SOURCES
    ${ARTNET_SRC_ROOT}/fileformat/ArtRecWriter.cpp
    ${ARTNET_SRC_ROOT}/fileformat/ArtRecReader.cpp
)

set(STORAGE_SOURCES
    ${ARTNET_SRC_ROOT}/storage/IStorageFormat.cpp
    ${ARTNET_SRC_ROOT}/storage/ArtRecStorage.cpp
    ${ARTNET_SRC_ROOT}/storage/JsonStorage.cpp
    ${ARTNET_SRC_ROOT}/storage/XmlStorage.cpp
    ${ARTNET_SRC_ROOT}/storage/StorageFactory.cpp
)

# ArtRecStorage 内部组合 ArtRecWriter/Reader, 所以也要链接 FILEFORMAT_SOURCES
set(STORAGE_ALL_SOURCES
    ${STORAGE_SOURCES}
    ${FILEFORMAT_SOURCES}
    ${CORE_SOURCES}
)

set(PROTOCOL_SOURCES
    ${ARTNET_SRC_ROOT}/protocol/IProtocolSource.cpp    # 空文件, 触发 AUTOMOC
    ${ARTNET_SRC_ROOT}/protocol/IProtocolSink.cpp      # 空文件
    ${ARTNET_SRC_ROOT}/protocol/ProtocolFactory.cpp
    # Source 实现 (ProtocolFactory::createSource 需要 new 这些)
    ${ARTNET_SRC_ROOT}/protocol/sources/ArtNetSource.cpp
    ${ARTNET_SRC_ROOT}/protocol/sources/NpuSource.cpp
    ${ARTNET_SRC_ROOT}/protocol/sources/Dmx512Source.cpp
    ${ARTNET_SRC_ROOT}/protocol/sources/ArtRecFileSource.cpp
    # Sink 实现 (ProtocolFactory::createSink 需要 new 这些)
    ${ARTNET_SRC_ROOT}/protocol/sinks/ArtNetSink.cpp
    ${ARTNET_SRC_ROOT}/protocol/sinks/NpuSink.cpp
    ${ARTNET_SRC_ROOT}/protocol/sinks/Dmx512Sink.cpp
)

# ArtRecFileSource 依赖 ArtRecReader
set(PROTOCOL_SOURCE_DEPS
    ${PROTOCOL_SOURCES}
    ${FILEFORMAT_SOURCES}
    ${NETWORK_SOURCES}
    ${OUTPUT_SOURCES}
    ${CORE_SOURCES}
)

# 同步抽象层: ISyncClock + 4 个时钟实现 + Factory
# VideoFileClock/AudioFileClock 依赖 FFmpegDecoder, 当前没装 FFmpeg, 先排除
set(SYNC_SOURCES
    ${ARTNET_SRC_ROOT}/sync/SyncTypes.h
    ${ARTNET_SRC_ROOT}/sync/ISyncClock.cpp
    ${ARTNET_SRC_ROOT}/sync/SyncClockFactory.cpp
    ${ARTNET_SRC_ROOT}/sync/clocks/InternalTimerClock.cpp
    ${ARTNET_SRC_ROOT}/sync/clocks/ArtNetTimeCodeClock.cpp
    ${ARTNET_SRC_ROOT}/sync/clocks/MidiMTCClock.cpp
    ${ARTNET_SRC_ROOT}/sync/clocks/TimecodeFileClock.cpp
    ${CORE_SOURCES}
)

# 每个 test 用同一个函数注册, 减少重复代码
# extraSources: 额外需要链接的 .cpp 文件 (相对路径或绝对路径)
function(add_unit_test name)
    # 解析额外源文件参数 (可选参数列表)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs EXTRA_SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    add_executable(${name} ${name}.cpp ${ARG_EXTRA_SOURCES})
    
    target_include_directories(${name} PRIVATE
        ${ARTNET_SRC_ROOT}
        ${ARTNET_SRC_ROOT}/core
        ${ARTNET_SRC_ROOT}/fileformat
        ${ARTNET_SRC_ROOT}/storage
        ${ARTNET_SRC_ROOT}/network
        ${ARTNET_SRC_ROOT}/media
        ${ARTNET_SRC_ROOT}/recorder
        ${ARTNET_SRC_ROOT}/output
        ${ARTNET_SRC_ROOT}/protocol
        ${ARTNET_SRC_ROOT}/sync
        ${ARTNET_SRC_ROOT}/sync/clocks
    )
    
    target_link_libraries(${name} PRIVATE
        Qt5::Test
        Qt5::Core
        Qt5::Network
        Qt5::SerialPort
        Qt5::Xml
    )
    
    # 如果启用了 FFmpeg, 测试也链接
    if(FFMPEG_LIBRARIES)
        target_link_libraries(${name} PRIVATE ${FFMPEG_LIBRARIES})
        target_include_directories(${name} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    endif()
    
    # 自动注册到 ctest
    add_test(NAME ${name} COMMAND ${name})

    # Windows: MidiMTCClock 使用 WinMM, 网络测试使用 Winsock
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32 winmm)
    endif()
    
    # 输出目录统一放到 build/tests/bin
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/bin
    )
endfunction()

# ============ Phase 1: 纯逻辑测试 (必须通过) ============
# TestCommon 只包含头文件内联函数, 不需要额外 .cpp
add_unit_test(TestCommon)

# TestProtocolFactory 需要 ProtocolFactory + Sources + Sinks (含依赖)
add_unit_test(TestProtocolFactory EXTRA_SOURCES ${PROTOCOL_SOURCE_DEPS})

# ============ Phase 2: 文件格式测试 (必须通过) ============
# TestArtRecFormat 需要 ArtRecWriter.cpp + ArtRecReader.cpp + Logger.cpp
add_unit_test(TestArtRecFormat
    EXTRA_SOURCES
        ${FILEFORMAT_SOURCES}
        ${CORE_SOURCES}
)

# ============ Phase 3: 协议 Source 层测试 (UDP 自环) ============
add_unit_test(TestProtocolSources EXTRA_SOURCES ${PROTOCOL_SOURCE_DEPS})

# ============ Phase 4: 协议 Sink 层测试 (UDP 自环) ============
add_unit_test(TestProtocolSinks EXTRA_SOURCES ${PROTOCOL_SOURCE_DEPS})

# ============ Phase 5: RecorderEngine 端到端集成测试 ============
set(ENGINE_SOURCES
    ${ARTNET_SRC_ROOT}/recorder/RecorderEngine.cpp
    ${ARTNET_SRC_ROOT}/output/OutputManager.cpp
    ${ARTNET_SRC_ROOT}/media/FFmpegDecoder.cpp
    ${FILEFORMAT_SOURCES}
    ${NETWORK_SOURCES}
    ${OUTPUT_SOURCES}
    ${CORE_SOURCES}
    # Engine 内部也依赖 Sink (OutputManager)
    ${PROTOCOL_SOURCES}
    # v1.3: RecorderEngine 已接入 ISyncClock 统一时间基准
    ${SYNC_SOURCES}
    # 跨格式管道测试需要 StorageFactory (artrec→json→xml)
    ${STORAGE_SOURCES}
)
add_unit_test(TestRecorderEngine EXTRA_SOURCES ${ENGINE_SOURCES})

# ============ Phase 6: Bug 回归测试 ============
# 覆盖 v1.2 修复的 8 个关键 Bug, 确保不复发
set(BUGREG_SOURCES
    # Bug #3, #7, #8: ArtRecWriter/Reader
    ${FILEFORMAT_SOURCES}
    ${CORE_SOURCES}
    # Bug #1: ArtRecWriter mutex 死锁 (只需要 FILEFORMAT)
    # Bug #4, #6: ArtNetReceiver 端口独占 + ArtDMX 包解析
    ${NETWORK_SOURCES}
    # Bug #2, #5: RecorderEngine state 机 + RecordState meta type
    ${ENGINE_SOURCES}
)
add_unit_test(TestBugRegressions EXTRA_SOURCES ${BUGREG_SOURCES})

# ============ Phase 7: 存储抽象层测试 ============
# 覆盖 StorageFactory + ArtRecStorage + JsonStorage + XmlStorage
# 含格式注册 / round-trip / 跨格式数据一致性 / seek
add_unit_test(TestStorageFormats EXTRA_SOURCES ${STORAGE_ALL_SOURCES})

# ============ Phase 8: 存储抽象层 × RecorderEngine 集成测试 ============
# 真实 UDP 自环 RecorderEngine 录制 → Storage 层跨格式读回验证
# 需要 ENGINE_SOURCES (RecorderEngine + UDP 接收) + STORAGE_ALL_SOURCES
set(STORAGE_INTEG_SOURCES
    ${STORAGE_ALL_SOURCES}
    ${ENGINE_SOURCES}
)
add_unit_test(TestStorageIntegration EXTRA_SOURCES ${STORAGE_INTEG_SOURCES})

# ============ Phase 9: 同步抽象层测试 ============
# 覆盖 SyncClockFactory + ISyncClock 契约 + 6 时钟实现 (InternalTimer/ArtNetTC/MidiMTC/TimecodeFile)
# VideoFile/AudioFile 需媒体文件依赖, 暂不测
add_unit_test(TestSyncClock EXTRA_SOURCES ${SYNC_SOURCES})

# ============ Phase 10: 同步抽象层集成测试 ============
# 跨模块: SyncClockFactory × ISyncClock × Storage 层 (ArtRecStorage/JsonStorage/XmlStorage) × UDP 自环
# 验证多时钟驱动 Storage 录制管道的端到端数据保真
set(SYNC_INTEG_SOURCES
    ${SYNC_SOURCES}
    ${STORAGE_ALL_SOURCES}
    ${ENGINE_SOURCES}
)
add_unit_test(TestSyncClockIntegration EXTRA_SOURCES ${SYNC_INTEG_SOURCES})
