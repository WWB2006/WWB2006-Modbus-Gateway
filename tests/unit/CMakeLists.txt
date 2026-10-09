find_package(Qt6 REQUIRED COMPONENTS Test)

foreach(test_name tst_crc16 tst_codec)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE gateway_core Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
