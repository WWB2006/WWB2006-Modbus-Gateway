# gateway_core 的源码分三组：
#   1) 完全不依赖 Qt 的核心（协议层、设备层、存储、模拟器、工具）；
#   2) 不依赖 Qt 的链路实现（链路抽象与接收缓冲）；
#   3) 依赖 Qt 的链路实现（阶段 2 为 TcpTransport，阶段 4 会加入 SerialTransport）。
# 这样在没有安装 Qt 的机器上也能构建 gateway_core 并运行 ctest，
# 与 README「方式三：CMake 已装但没有 Qt」的描述一致。

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    protocol/*.cpp
    device/*.cpp
    storage/*.cpp
    sim/*.cpp
    util/*.cpp)

# 不依赖 Qt 的链路实现，随核心库一起编译
set(TRANSPORT_CORE_SOURCES
    transport/TransportInterface.cpp
    transport/ByteAccumulator.cpp)

# 依赖 Qt6::Network 的链路实现，只有在检测到 Qt 时才加入
set(TRANSPORT_QT_SOURCES
    transport/TcpTransport.cpp)

foreach(src IN LISTS TRANSPORT_CORE_SOURCES)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
        list(APPEND CORE_SOURCES "${src}")
    endif()
endforeach()

if (NOT CORE_SOURCES)
    message(FATAL_ERROR "src/ 下没有找到核心源码，检查目录是否被清空")
endif()

add_library(gateway_core STATIC ${CORE_SOURCES})
target_include_directories(gateway_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

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
    file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
    add_executable(modbus_gateway WIN32 main.cpp ${UI_SOURCES})
    target_link_libraries(modbus_gateway PRIVATE gateway_core Qt6::Widgets)
endif()
