target_sources(proto_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ControllerLink.cpp)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIM_EPOXY REQUIRED IMPORTED_TARGET epoxy)
find_package(glm CONFIG REQUIRED)

add_library(protomate_simulation_machine STATIC
    ${PROJECT_SOURCE_DIR}/platform/simulation/SharedMemory.cpp
    Machine.cpp Peripherals.cpp Media.cpp SimulatedStepHardware.cpp Tmc2209Model.cpp)
target_include_directories(protomate_simulation_machine PUBLIC ${PROJECT_SOURCE_DIR}/platform ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(protomate_simulation_machine PUBLIC project_compiler_settings)

add_library(protomate_simulation_bridge STATIC
    ../common/icc/bridge/Bridge.cpp ../common/icc/bridge/ConnectionHealth.cpp
    ../common/icc/bridge/TopicBridge.cpp ../common/icc/bridge/ServiceBridge.cpp
    ../common/icc/bridge/ActionBridge.cpp ../common/icc/bridge/Frame.cpp)
target_include_directories(protomate_simulation_bridge PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(protomate_simulation_bridge PUBLIC project_compiler_settings pneumo::coroutines pneumo::messaging)
add_library(protomate_simulation_session STATIC Session.cpp)
target_link_libraries(protomate_simulation_session PUBLIC protomate_simulation_machine protomate_simulation_bridge pneumo::messaging Threads::Threads)

add_executable(protomate-sim main.cpp viewer/Viewer.cpp)
set_target_properties(protomate-sim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_link_libraries(protomate-sim PRIVATE protomate_simulation_session PkgConfig::SDL2 PkgConfig::SIM_EPOXY glm::glm)
add_custom_target(run-simulation COMMAND $<TARGET_FILE:protomate-sim> --controller $<TARGET_FILE:Application>
    DEPENDS protomate-sim Application USES_TERMINAL)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(protomate_simulation_gl_tests tests/gl_test.cpp)
    target_link_libraries(protomate_simulation_gl_tests PRIVATE project_compiler_settings PkgConfig::SDL2 PkgConfig::SIM_EPOXY glm::glm)
    add_executable(protomate_simulation_storage_probe tests/storage_probe.cpp)
    runtime_attach(protomate_simulation_storage_probe)
    target_include_directories(protomate_simulation_storage_probe PRIVATE ${PROJECT_SOURCE_DIR}/platform)
    add_test(NAME simulation.storage COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/storage.py $<TARGET_FILE:protomate-sim> $<TARGET_FILE:protomate_simulation_storage_probe>)
    set_tests_properties(simulation.storage PROPERTIES TIMEOUT 125)
    add_test(NAME simulation.lifecycle COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle.py $<TARGET_FILE:protomate-sim> $<TARGET_FILE:Application>)
    set_tests_properties(simulation.lifecycle PROPERTIES TIMEOUT 60)
    find_program(SIM_XVFB_RUN xvfb-run)
    if(SIM_XVFB_RUN)
        add_test(NAME simulation.opengl COMMAND ${SIM_XVFB_RUN} -a $<TARGET_FILE:protomate_simulation_gl_tests>)
        set_tests_properties(simulation.opengl PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;SDL_VIDEODRIVER=x11")
    endif()
    add_executable(protomate_simulation_tests tests/machine_test.cpp)
    target_link_libraries(protomate_simulation_tests PRIVATE protomate_simulation_machine)
    add_test(NAME simulation.machine COMMAND protomate_simulation_tests)
    set_tests_properties(simulation.machine PROPERTIES TIMEOUT 15)
    add_test(NAME simulation.integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:protomate-sim> $<TARGET_FILE:Application>)
    set_tests_properties(simulation.integration PROPERTIES TIMEOUT 90)
endif()
