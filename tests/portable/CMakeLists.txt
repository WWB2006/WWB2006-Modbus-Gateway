foreach(test_name portable_tests device_tests)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE gateway_core)

    # 不依赖 Qt，所以任何环境下都能加入 ctest
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
