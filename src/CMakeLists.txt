# gateway_core 的源码分三组：
#   1) 完全不依赖 Qt 的核心（协议层、设备层、存储、模拟器、工具）；
#   2) 不依赖 Qt 的链路实现（链路抽象与接收缓冲）；
#   3) 依赖 Qt 的链路实现（阶段 2 为 TcpTransport，阶段 4 会加入 SerialTransport）。
# 这样在没有安装 Qt 的机器上也能构建 gateway_core 并运行 ctest，
# 与 README「方式三：CMake 已装但没有 Qt」的描述一致。

# 收集 src/ 下的全部 .cpp，再把「由下方按 Qt 模块条件编译」的几组剔除。
#
# 为什么用整体 glob 而不是逐目录列举（protocol/ device/ storage/ …）：
#   逐目录列举漏掉一个目录时，失败方式是链接期一大堆 undefined reference，
#   而 CMake 不会提示「你漏了哪个目录」。阶段 11 新增 src/metrics/ 时就踩过一次：
#   可移植脚本能过（它手写源文件列表），CMake 构建却链接失败。
#   整体 glob 之后，新增目录零维护 —— 这类"加了新目录却忘了改构建脚本"
#   是最容易在提交后才被发现的问题。
file(GLOB ALL_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")

set(CORE_SOURCES "")
foreach(src IN LISTS ALL_SOURCE_FILES)
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${src}")
    # ui/ 整目录需要 Qt6::Widgets；transport/ 下的三个 Qt 实现各需要对应模块。
    # 它们按模块是否存在条件加入，因此不能进"无条件核心库"。
    if (rel MATCHES "^ui/")
        continue()
    endif()
    if (rel STREQUAL "transport/TcpTransport.cpp"
        OR rel STREQUAL "transport/SerialTransport.cpp"
        OR rel STREQUAL "transport/TransportFactory.cpp")
        continue()
    endif()
    list(APPEND CORE_SOURCES "${src}")
endforeach()

# 依赖 Qt6::Network 的链路实现，只有在检测到 Qt 时才加入
set(TRANSPORT_QT_SOURCES
    transport/TcpTransport.cpp
    transport/TransportFactory.cpp)

# 依赖 Qt6::SerialPort 的链路实现（阶段 4：RTU 串口）。
# 单独一份列表：没有 Qt SerialPort 模块时不能把它编进核心库，
# 否则会因找不到 <QSerialPort> 而整个构建失败。
set(TRANSPORT_SERIAL_SOURCES
    transport/SerialTransport.cpp)

if (NOT CORE_SOURCES)
    message(FATAL_ERROR "src/ 下没有找到核心源码，检查目录是否被清空")
endif()

# 阶段 6：vendored 的 SQLite amalgamation。
#
# 为什么放在 gateway_core 里而不是单独建一个静态库：
#   只有 storage/ 用到它，单独建库只是多一个 target 名字；
#   而 sqlite3.c 必须由 **C 编译器** 编译（当 C++ 编会因 void* 隐式转换、
#   sqlite3WhereTrace 重定义等失败），所以关键在于它的扩展名是 .c
#   且项目启用了 C 语言 —— 这两点比放在哪个 target 更重要。
#
# 编译选项与 scripts/build_portable_tests.sh 保持一致：
#   两个构建路径（CMake 与可移植脚本）编出的 sqlite 必须同源同配置，
#   否则会出现「脚本里能过、CMake 里行为不同」这种最难查的差异。
set(SQLITE_SOURCE "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
if (EXISTS "${SQLITE_SOURCE}")
    list(APPEND CORE_SOURCES "${SQLITE_SOURCE}")
    # sqlite3.c 是第三方代码，不参与本项目的 -Wall -Wextra 门禁：
    # 它在 GCC 13 / MSVC 19 上都会产生大量无害警告，把真正的警告淹掉。
    # 注意 set_source_files_properties 必须在 add_library 之前 —— 它按路径登记属性，
    # 而 target 的 include/definitions 必须在 add_library **之后**（target 还不存在）。
    set_source_files_properties("${SQLITE_SOURCE}" PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_library(gateway_core STATIC ${CORE_SOURCES})
target_include_directories(gateway_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

if (EXISTS "${SQLITE_SOURCE}")
    target_include_directories(gateway_core PRIVATE "${CMAKE_SOURCE_DIR}/third_party/sqlite")
    target_compile_definitions(gateway_core PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_COLUMN_METADATA
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
        SQLITE_ENABLE_RTREE
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_DQS=0
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_SHARED_CACHE)
    message(STATUS "已启用 vendored SQLite：third_party/sqlite/sqlite3.c")
else()
    message(WARNING "缺少 third_party/sqlite/sqlite3.c：SqliteStorage 将无法链接。"
                    "存储层请改用 MemoryStorage，或补回 vendored 源码。")
endif()

# 阶段 3 的从站模拟器直接用 WinSock（刻意不依赖 Qt），Windows 下要显式链接 ws2_32。
if (WIN32)
    target_link_libraries(gateway_core PUBLIC ws2_32)
endif()

# 协议层与设备层不依赖 Qt；只有接入网络链路时才需要 Qt6::Network。
if (TARGET Qt6::Network)
    set(_qt_transport_found FALSE)
    foreach(src IN LISTS TRANSPORT_QT_SOURCES)
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
            target_sources(gateway_core PRIVATE "${src}")
            set(_qt_transport_found TRUE)
        endif()
    endforeach()
    if (TARGET Qt6::Core)
        target_link_libraries(gateway_core PUBLIC Qt6::Core Qt6::Network)
    else()
        target_link_libraries(gateway_core PUBLIC Qt6::Network)
    endif()
    if (_qt_transport_found)
        message(STATUS "已启用 Qt 链路实现：${TRANSPORT_QT_SOURCES}")
    endif()
elseif (TARGET Qt6::Core)
    target_link_libraries(gateway_core PUBLIC Qt6::Core)
else()
    message(STATUS "未检测到 Qt6::Network：跳过 TcpTransport，只构建不依赖 Qt 的核心。")
endif()

# 阶段 4：串口 RTU 链路。只有装了 Qt SerialPort 模块（Ubuntu 上包名
# libqt6serialport6-dev，Windows 用 Qt Online Installer 勾选 SerialPort）才编入。
# 与 TcpTransport 独立判断：允许「有 Network 没 SerialPort」的机器。
if (TARGET Qt6::SerialPort)
    set(_qt_serial_found FALSE)
    foreach(src IN LISTS TRANSPORT_SERIAL_SOURCES)
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
            target_sources(gateway_core PRIVATE "${src}")
            set(_qt_serial_found TRUE)
        endif()
    endforeach()
    target_link_libraries(gateway_core PUBLIC Qt6::SerialPort)
    # SerialTransport.cpp 用这个宏决定是否编入串口分支；
    # TransportFactory.cpp 也用它决定「请求串口链路时是返回实现还是报错」。
    # 宏定义挂在 core 上并用 PUBLIC + INTERFACE 一起传：
    # 单用 PRIVATE 会让 TransportFactory.cpp（同属 core）也看不到，导致串口永远不可用。
    target_compile_definitions(gateway_core PRIVATE GATEWAY_HAS_SERIALPORT=1)
    if (_qt_serial_found)
        message(STATUS "已启用 Qt 串口链路实现：${TRANSPORT_SERIAL_SOURCES}")
    endif()
else()
    message(STATUS "未检测到 Qt6::SerialPort：跳过 SerialTransport（RTU 串口不可用）。")
endif()

add_executable(gateway_cli gateway_cli.cpp)
target_link_libraries(gateway_cli PRIVATE gateway_core)

# 阶段 3：从站模拟器。只用 POSIX/WinSock 套接字，不依赖 Qt，
# 因此没装 Qt 的机器上也能编译并用来做交叉验证。
add_executable(slave_simulator sim/slave_simulator_main.cpp)
target_link_libraries(slave_simulator PRIVATE gateway_core)

if (WIN32)
    target_link_libraries(slave_simulator PRIVATE ws2_32)
endif()

if (TARGET Qt6::Widgets)
    # 阶段 5 的界面层：主窗口 + 寄存器表模型。
    # 只有装了 Qt Widgets 才编 —— 无图形环境的机器上不构建界面程序，
    # 但 gateway_core / gateway_cli / slave_simulator 仍然可用（与 README 一致）。
    file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
    add_executable(modbus_gateway WIN32 main.cpp ${UI_SOURCES})
    target_link_libraries(modbus_gateway PRIVATE gateway_core Qt6::Widgets)
    message(STATUS "已启用 Qt 界面程序：modbus_gateway（阶段 5）")
endif()

# 阶段 11：无界面常驻网关（gateway_daemon）。
# 它不依赖任何 Qt 模块，因此任何平台都能构建 —— 这正是「无图形环境稳定运行」的保证。
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gateway_daemon.cpp")
    add_executable(gateway_daemon gateway_daemon.cpp)
    target_link_libraries(gateway_daemon PRIVATE gateway_core)
    if (NOT TARGET Qt6::Network)
        message(STATUS "gateway_daemon 已构建（当前构建无 Qt，因此只支持模拟器/假链路场景）。")
    endif()
endif()
