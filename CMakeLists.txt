cmake_minimum_required(VERSION 3.16)
# 需要 C 语言：third_party/sqlite/sqlite3.c 是 C 代码，
# 且必须用 C 编译器编（当 C++ 编会因 void* 隐式转换、redefinition 等失败）。
project(modbus_gateway LANGUAGES CXX C)

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
    # 用 OPTIONAL_COMPONENTS：缺少 Test 这类非必需模块时，
    # 不应把 Qt6_FOUND 置为假，否则会误报「未找到 Qt6」。
    #
    # 这里只列当前阶段真正会被 target 链接的模块：
    #   Core     —— 协议层 / 设备层基础类型
    #   Network  —— src/transport/TcpTransport.cpp
    #   Widgets  —— src/main.cpp 界面程序 modbus_gateway
    #   Test     —— tests/unit 下的 tst_crc16 / tst_codec
    #
    # 注意：OPTIONAL_COMPONENTS 只保证「整个 Qt6 缺失时不报错」，
    # 对子模块自身 find_package 抛出的 FATAL_ERROR 没有保护作用。
    # 阶段 4 需要 Qt6::SerialPort（QSerialPort）。
    # ⚠️ OPTIONAL_COMPONENTS 只保证「整个 Qt6 缺失时不报错」，
    # 对子模块自身 find_package 抛出的 FATAL_ERROR 没有保护作用。
    # 阶段 4 才需要的 SerialBus / Charts，以及本项目从未使用的 Sql，
    # 其 CMake 配置文件会校验 canbusutil、qmlplugindump 等附带工具，
    # 任一缺失即 FATAL_ERROR 中断整个配置（Ubuntu 的 libqt6serialbus6-plugins
    # 就缺 /usr/lib/qt6/bin/canbusutil 这个文件）。
    # 因此这里只列 SerialPort（它没有这类附带工具依赖），绝不写 SerialBus。
    find_package(Qt6 6.2 QUIET OPTIONAL_COMPONENTS
        Widgets Network Test SerialPort)
    if (TARGET Qt6::SerialPort)
        message(STATUS "检测到 Qt6::SerialPort：启用串口 RTU 链路（SerialTransport）。")
    else()
        message(STATUS "未检测到 Qt6::SerialPort：跳过 SerialTransport，串口链路不可用。")
    endif()
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
