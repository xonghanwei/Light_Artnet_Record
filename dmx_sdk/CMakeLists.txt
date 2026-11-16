# dmx_sdk/CMakeLists.txt — DMX512 串口读写第三方 SDK DLL (仅 Qt Core + SerialPort)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core SerialPort)

add_library(dmx512_sdk SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Dmx512Sdk.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Dmx512Sdk.h
    # 复用的生产源码 (发送 / 接收 / 日志)
    ${CMAKE_SOURCE_DIR}/src/output/Dmx512Sender.cpp
    ${CMAKE_SOURCE_DIR}/src/output/Dmx512Sender.h
    ${CMAKE_SOURCE_DIR}/src/protocol/sources/Dmx512Source.cpp
    ${CMAKE_SOURCE_DIR}/src/protocol/sources/Dmx512Source.h
    ${CMAKE_SOURCE_DIR}/src/protocol/IProtocolSource.h
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_compile_definitions(dmx512_sdk PRIVATE DMX512_SDK_BUILD)
target_include_directories(dmx512_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/output
    ${CMAKE_SOURCE_DIR}/src/protocol
    ${CMAKE_SOURCE_DIR}/src/protocol/sources
)
target_link_libraries(dmx512_sdk PRIVATE Qt5::Core Qt5::SerialPort)

set_target_properties(dmx512_sdk PROPERTIES
    PREFIX ""                                   # dmx512_sdk.dll
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# 第三方调用方式演示 + 自动化自检 (纯 Win32 动态加载, 不链接 SDK)
add_executable(dmx512_sdk_test test/main.cpp)
set_target_properties(dmx512_sdk_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
add_dependencies(dmx512_sdk_test dmx512_sdk)

message(STATUS "  dmx512_sdk — DMX512 串口读写 C API DLL (dmx512_sdk.dll)")
message(STATUS "  dmx512_sdk_test — 动态加载自检程序")
