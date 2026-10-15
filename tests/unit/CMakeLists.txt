# Unit Test Executables
set(TEST_CASES
    tst_crc
    tst_codec
    tst_reconnect
    tst_storage
    tst_scheduler
)

foreach(test_name ${TEST_CASES})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE gateway_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
