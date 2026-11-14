# tests/CMakeLists.txt

option(BUILD_TESTING "Build unit tests" ON)
option(BUILD_UI_TESTING "Build UI functional test panels (手动运行, 不进 ctest)" ON)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(unit)
    add_subdirectory(integration)
endif()

if(BUILD_UI_TESTING)
    add_subdirectory(ui)
endif()
