# SQLite C Library
add_library(sqlite3_lib STATIC ${CMAKE_SOURCE_DIR}/third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite3)

# Core library source files
set(CORE_SOURCES
    protocol/Crc16.cpp
    protocol/ModbusCodec.cpp
    transport/TcpTransport.cpp
    transport/SerialTransport.cpp
    device/ReconnectPolicy.cpp
    device/DeviceManager.cpp
    device/PollScheduler.cpp
    storage/SqliteStorage.cpp
    sim/FaultInjector.cpp
    sim/SlaveSimulator.cpp
    util/Logger.cpp
    util/Metrics.cpp
    util/AppConfig.cpp
)

add_library(gateway_core STATIC ${CORE_SOURCES})
target_include_directories(gateway_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/sqlite3
)

target_link_libraries(gateway_core PUBLIC sqlite3_lib)

if(WIN32)
    target_link_libraries(gateway_core PUBLIC ws2_32)
else()
    target_link_libraries(gateway_core PUBLIC pthread dl)
endif()

# Headless Gateway CLI Daemon
add_executable(gateway_cli gateway_cli.cpp)
target_link_libraries(gateway_cli PRIVATE gateway_core)

# Standalone Slave Simulator CLI
add_executable(slave_simulator slave_main.cpp)
target_link_libraries(slave_simulator PRIVATE gateway_core)

# Qt GUI Application (when Qt6 is present)
if(Qt6_FOUND)
    file(GLOB_RECURSE UI_SOURCES ui/*.cpp)
    add_executable(modbus_gateway WIN32 main.cpp ${UI_SOURCES})
    target_link_libraries(modbus_gateway PRIVATE gateway_core Qt6::Widgets)
    if(TARGET Qt6::Charts)
        target_link_libraries(modbus_gateway PRIVATE Qt6::Charts)
    endif()
endif()
