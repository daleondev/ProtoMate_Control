# Reuse the native GoogleTest build; these tests do not link the ThreadX ABI.
add_executable(motion_bench_metrics_tests
    metrics_test.cpp crossing_test.cpp compact_test.cpp fixed_test.cpp sequence_test.cpp ../CompactExpand.cpp
    ${PROJECT_SOURCE_DIR}/src/common/motion/Profile.cpp)
target_include_directories(motion_bench_metrics_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(motion_bench_metrics_tests PRIVATE hal_register_gtest project_compiler_settings)
add_test(NAME motion_bench.metrics COMMAND motion_bench_metrics_tests)
set_tests_properties(motion_bench.metrics PROPERTIES TIMEOUT 30)
