# tests/integration/CMakeLists.txt
#
# 集成测试: 多个模块协同, 需要真实 UDP / 文件 / 时间码
# Phase 1+2 不涉及, 预留框架

find_package(Qt5 REQUIRED COMPONENTS Test Core Network)

# 暂时只占位, 没有实际集成测试 target
# Phase 5 开始时加入:
#   add_executable(TestRecorderEngine TestRecorderEngine.cpp)
#   target_link_libraries(...)
#   add_test(NAME TestRecorderEngine COMMAND TestRecorderEngine)
