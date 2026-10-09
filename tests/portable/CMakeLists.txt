add_executable(portable_tests portable_tests.cpp)
target_link_libraries(portable_tests PRIVATE gateway_core)

# 不依赖 Qt，所以任何环境下都能加入 ctest
add_test(NAME portable_tests COMMAND portable_tests)
