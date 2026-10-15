foreach(test_name portable_tests device_tests simulator_tests serial_tests config_tests
                   storage_tests fault_tests recovery_tests native_transport_tests)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE gateway_core)

    # 不依赖 Qt，所以任何环境下都能加入 ctest。
    #
    # 把仓库根目录作为第一个参数传进去：用例需要读仓库里的真实文件
    # （如 config/gateway.json）时靠它拼绝对路径。
    # 不让测试靠当前工作目录猜 —— ctest 的工作目录是
    # build/<cfg>/tests/portable/，与构建脚本的 build_portable/ 不同，
    # 靠猜会出现「脚本全绿、ctest 红」这种同一份代码两套结果的问题。
    add_test(NAME ${test_name} COMMAND ${test_name} "${CMAKE_SOURCE_DIR}")
endforeach()
