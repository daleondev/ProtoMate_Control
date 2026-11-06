set(bench_sources Bench.cpp Kernels.cpp Communication.cpp Trace.cpp Pulses.cpp Lifecycles.cpp Isolation.cpp CompactExpand.cpp FastControl.cpp FixedBench.cpp ProfileBench.cpp ExecutorBench.cpp)
target_sources(${PROJECT_NAME} PRIVATE ${bench_sources}
    ${PROJECT_SOURCE_DIR}/src/io/motion/Executor.cpp
    ${PROJECT_SOURCE_DIR}/src/io/motion/ExecutorDiagnostic.cpp
    ${PROJECT_SOURCE_DIR}/src/io/motion/ProfileSchedule.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/motion/AxisConversion.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/motion/ScaraKinematics.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/motion/RobotConfiguration.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/motion/Planner.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE proto::common)
# Keep the timed command loop and its instantiated ICC routes at production
# optimization in Debug too. Unoptimized benchmark glue on M4's uncached QSPI
# can starve renewal and heartbeats even though the executor itself is optimized.
# Debug symbols/assertions and the application's remaining Debug code are retained.
foreach(source IN LISTS bench_sources)
    set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        TARGET_DIRECTORY ${PROJECT_NAME} APPEND PROPERTY COMPILE_OPTIONS $<$<CONFIG:Debug>:-Os>)
endforeach()
# CPU kernels are evaluated with production optimization even in the debug image.
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Kernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FixedBench.cpp
    TARGET_DIRECTORY ${PROJECT_NAME} APPEND PROPERTY COMPILE_OPTIONS -O2)
if(application_role STREQUAL "ui")
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/ui/gui ${PROJECT_BINARY_DIR}/gui)
    target_link_libraries(${PROJECT_NAME} PRIVATE proto::ui::gui)
endif()
