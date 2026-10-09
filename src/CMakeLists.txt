file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    protocol/*.cpp
    transport/*.cpp
    device/*.cpp
    storage/*.cpp
    sim/*.cpp
    util/*.cpp)

if (NOT CORE_SOURCES)
    message(FATAL_ERROR "src/ 下没有找到核心源码，检查目录是否被清空")
endif()

add_library(gateway_core STATIC ${CORE_SOURCES})
target_include_directories(gateway_core PUBLIC ${CMAKE_SOURCE_DIR}/include)

# 协议层不依赖 Qt；只有后续阶段接入网络与数据库时才需要链接 Qt 模块。
if (TARGET Qt6::Core)
    target_link_libraries(gateway_core PUBLIC Qt6::Core)
endif()

add_executable(gateway_cli gateway_cli.cpp)
target_link_libraries(gateway_cli PRIVATE gateway_core)

if (TARGET Qt6::Widgets)
    file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
    add_executable(modbus_gateway WIN32 main.cpp ${UI_SOURCES})
    target_link_libraries(modbus_gateway PRIVATE gateway_core Qt6::Widgets)
endif()
