cmake_minimum_required(VERSION 3.16)
project(modbus_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# 阶段 0-1 的核心只有协议层，只需要 Qt6::Core。
# 这里故意把 Qt 设为可选：没装 Qt 时仍能构建并测试无界面核心，
# 装好 Qt 6 后重新执行 cmake，界面与 Qt Test 目标会自动出现。
find_package(Qt6 6.2 QUIET COMPONENTS Core)

if (Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    find_package(Qt6 6.2 QUIET COMPONENTS Widgets Network SerialPort SerialBus Sql Charts Test)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests/portable)

if (TARGET Qt6::Test)
    add_subdirectory(tests/unit)
endif()

if (NOT Qt6_FOUND)
    message(STATUS "未找到 Qt6：只构建 gateway_core 与 gateway_cli（协议层）。")
    message(STATUS "装好 Qt 6 后重新执行 cmake，会自动启用界面程序与 Qt Test。")
endif()
