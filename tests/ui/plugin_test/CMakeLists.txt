# tests/ui/plugin_test/CMakeLists.txt — DLL 插件测试台
find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)

add_executable(artnet_ui_plugin_test WIN32 main.cpp)
target_link_libraries(artnet_ui_plugin_test PRIVATE Qt5::Widgets Qt5::Core)
set_target_properties(artnet_ui_plugin_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)
# 先构建插件, 保证测试台运行时 DLL 已存在
add_dependencies(artnet_ui_plugin_test artnet_ui_plugin)

message(STATUS "  artnet_ui_plugin_test — DLL 插件测试台 (手动运行)")
