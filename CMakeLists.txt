cmake_minimum_required(VERSION 3.16)
project(modbus_gateway LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Detect Qt6 optional components
find_package(Qt6 6.2 COMPONENTS Core Widgets Network SerialPort Sql Test QUIET)
find_package(Qt6 COMPONENTS Charts QUIET)

if(Qt6_FOUND)
    message(STATUS "Qt6 found (${Qt6_VERSION}). Full Qt GUI enabled.")
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
else()
    message(STATUS "Qt6 not found. Building in headless/daemon mode (gateway_cli, simulator, unit tests).")
endif()

if(MSVC)
    add_compile_options(/W3 /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests/unit)
