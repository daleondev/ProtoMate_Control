target_sources(proto_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ControllerLink.cpp)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SIM_EPOXY REQUIRED IMPORTED_TARGET epoxy)
find_package(glm CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)

# Keep the 50 MiB model byte initializers out of host DWARF, as for the controller image.
set_source_files_properties(assets/RobotModels.cpp assets/CollisionMeshes.cpp PROPERTIES COMPILE_OPTIONS "-g0")
add_library(protomate_simulation_assets STATIC assets/RobotModels.cpp assets/Shaders.cpp assets/CollisionMeshes.cpp)
target_link_libraries(protomate_simulation_assets PUBLIC project_compiler_settings)
add_library(protomate_simulation_kinematics STATIC Profile.cpp Kinematics.cpp ../ui/motion/AxisConversion.cpp)
target_include_directories(protomate_simulation_kinematics PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(protomate_simulation_kinematics PUBLIC project_compiler_settings pneumo::units glm::glm)
add_library(protomate_simulation_contacts STATIC CollisionMesh.cpp ContactSensors.cpp)
target_link_libraries(protomate_simulation_contacts PUBLIC protomate_simulation_kinematics PRIVATE protomate_simulation_assets)
add_library(protomate_simulation_models STATIC viewer/ModelAsset.cpp)
target_link_libraries(protomate_simulation_models PUBLIC project_compiler_settings glm::glm protomate_simulation_kinematics PRIVATE assimp::assimp protomate_simulation_assets)
add_library(protomate_simulation_model_renderer STATIC viewer/ModelRenderer.cpp)
target_link_libraries(protomate_simulation_model_renderer PUBLIC protomate_simulation_models PkgConfig::SDL2 PkgConfig::SIM_EPOXY)

add_library(protomate_simulation_machine STATIC
    ${PROJECT_SOURCE_DIR}/platform/simulation/SharedMemory.cpp
    Machine.cpp Peripherals.cpp Media.cpp SimulatedStepHardware.cpp Tmc2209Model.cpp)
target_include_directories(protomate_simulation_machine PUBLIC ${PROJECT_SOURCE_DIR}/platform ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(protomate_simulation_machine PUBLIC project_compiler_settings protomate_simulation_contacts)

add_library(protomate_simulation_bridge STATIC
    ../common/icc/bridge/Bridge.cpp ../common/icc/bridge/ConnectionHealth.cpp
    ../common/icc/bridge/TopicBridge.cpp ../common/icc/bridge/ServiceBridge.cpp
    ../common/icc/bridge/ActionBridge.cpp ../common/icc/bridge/Frame.cpp)
target_include_directories(protomate_simulation_bridge PUBLIC ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(protomate_simulation_bridge PUBLIC project_compiler_settings pneumo::coroutines pneumo::messaging platform::profiling_unavailable)
add_library(protomate_simulation_session STATIC Session.cpp ControllerProcess.cpp)
target_link_libraries(protomate_simulation_session PUBLIC protomate_simulation_machine protomate_simulation_bridge pneumo::messaging Threads::Threads)

add_library(protomate_simulation_image STATIC ExecutableImage.cpp)
target_link_libraries(protomate_simulation_image PUBLIC project_compiler_settings)

# The compiler embeds the complete private ELF as read-only C++26 data.
# Track the generated ELF explicitly so changed controller code rebuilds the asset.
set_source_files_properties(ControllerImage.cpp PROPERTIES
    OBJECT_DEPENDS "${PROTOMATE_CONTROLLER_IMAGE}"
    # Avoid describing the entire byte initializer again in host DWARF.
    # The embedded controller ELF retains its own complete debug information.
    COMPILE_OPTIONS "-g0")
add_executable(Application main.cpp viewer/Viewer.cpp ControllerImage.cpp)
target_compile_definitions(Application PRIVATE
    PROTOMATE_CONTROLLER_IMAGE="${PROTOMATE_CONTROLLER_IMAGE}")
add_dependencies(Application ${PROTOMATE_CONTROLLER_TARGET})
set_target_properties(Application PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
target_link_libraries(Application PRIVATE protomate_simulation_session protomate_simulation_image protomate_simulation_model_renderer PkgConfig::SDL2 PkgConfig::SIM_EPOXY glm::glm)
foreach(target Application protomate_simulation_machine protomate_simulation_session
               protomate_simulation_image protomate_simulation_bridge
               protomate_simulation_models protomate_simulation_model_renderer protomate_simulation_assets
               protomate_simulation_kinematics protomate_simulation_contacts)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
endforeach()
add_custom_target(run-simulation COMMAND $<TARGET_FILE:Application>
    DEPENDS Application USES_TERMINAL)

if(BUILD_TESTING)
    # The existing GTest targets link our ThreadX runtime. Native simulator tests
    # must use the host C++ ABI, just like Application. Reuse the pinned source,
    # but compile a separate copy without any runtime/HAL dependencies.
    include(FetchContent)
    FetchContent_GetProperties(googletest)
    add_library(protomate_simulation_gtest STATIC ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc)
    target_include_directories(protomate_simulation_gtest SYSTEM PUBLIC
        ${googletest_SOURCE_DIR}/googletest/include)
    target_include_directories(protomate_simulation_gtest PRIVATE ${googletest_SOURCE_DIR}/googletest)
    target_link_libraries(protomate_simulation_gtest PUBLIC Threads::Threads)
    add_library(protomate_simulation_gtest_main STATIC ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc)
    target_link_libraries(protomate_simulation_gtest_main PUBLIC protomate_simulation_gtest)
    add_executable(protomate_simulation_display_clock_tests tests/display_clock_test.cpp)
    target_link_libraries(protomate_simulation_display_clock_tests PRIVATE
        protomate_simulation_gtest_main hal_linux_display PkgConfig::SDL2)
    add_test(NAME simulation.display_clock COMMAND protomate_simulation_display_clock_tests)
    set_tests_properties(simulation.display_clock PROPERTIES TIMEOUT 15 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import pytest"
        RESULT_VARIABLE SIM_PYTEST_STATUS OUTPUT_QUIET ERROR_QUIET)
    if(NOT SIM_PYTEST_STATUS EQUAL 0)
        message(FATAL_ERROR "Simulator tests require pytest; install scripts/requirements.txt with ${Python3_EXECUTABLE}")
    endif()
    function(add_simulation_pytest name file)
        add_test(NAME simulation.${name} COMMAND ${Python3_EXECUTABLE} -m pytest -q -ra
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/${file}
            -o cache_dir=${PROJECT_BINARY_DIR}/pytest-cache/simulation
            --junitxml=${PROJECT_BINARY_DIR}/test-results/simulation-${name}.xml
            --simulator=$<TARGET_FILE:Application>
            --controller=$<TARGET_FILE:${PROTOMATE_CONTROLLER_TARGET}>
            --launcher=$<TARGET_FILE:protomate_simulation_test_launcher>
            --client-probe=$<TARGET_FILE:protomate_simulation_client_probe>
            --storage-probe=$<TARGET_FILE:protomate_simulation_storage_probe>
            --nm=${CMAKE_NM} ${ARGN})
    endfunction()
    add_simulation_pytest(cad_assets test_cad_assets.py)
    set_tests_properties(simulation.cad_assets PROPERTIES TIMEOUT 15)
    add_simulation_pytest(architecture test_architecture.py
        --native-test=$<TARGET_FILE:protomate_simulation_contact_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_review_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_session_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_model_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_gl_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_launch_tests>
        --native-test=$<TARGET_FILE:protomate_simulation_tests>)
    set_tests_properties(simulation.architecture PROPERTIES TIMEOUT 30)
    add_executable(protomate_simulation_contact_tests tests/contact_test.cpp)
    target_link_libraries(protomate_simulation_contact_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_contacts)
    add_test(NAME simulation.contacts COMMAND protomate_simulation_contact_tests)
    set_tests_properties(simulation.contacts PROPERTIES TIMEOUT 30)
    add_executable(protomate_simulation_review_tests tests/review_test.cpp)
    target_link_libraries(protomate_simulation_review_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_machine)
    add_test(NAME simulation.regressions COMMAND protomate_simulation_review_tests)
    set_tests_properties(simulation.regressions PROPERTIES TIMEOUT 15)
    add_executable(protomate_simulation_session_tests tests/session_test.cpp)
    target_link_libraries(protomate_simulation_session_tests PRIVATE protomate_simulation_gtest protomate_simulation_session)
    add_test(NAME simulation.session COMMAND protomate_simulation_session_tests)
    set_tests_properties(simulation.session PROPERTIES TIMEOUT 15)
    add_executable(protomate_simulation_model_tests tests/model_test.cpp)
    target_link_libraries(protomate_simulation_model_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_models)
    add_test(NAME simulation.models COMMAND protomate_simulation_model_tests)
    set_tests_properties(simulation.models PROPERTIES TIMEOUT 30)
    add_executable(protomate_simulation_gl_tests tests/gl_test.cpp)
    target_link_libraries(protomate_simulation_gl_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_model_renderer project_compiler_settings PkgConfig::SDL2 PkgConfig::SIM_EPOXY glm::glm)
    add_executable(protomate_simulation_client_probe tests/client_probe.cpp)
    runtime_attach(protomate_simulation_client_probe)
    target_include_directories(protomate_simulation_client_probe PRIVATE ${PROJECT_SOURCE_DIR}/platform)
    add_simulation_pytest(client test_client.py)
    set_tests_properties(simulation.client PROPERTIES TIMEOUT 90)
    add_executable(protomate_simulation_storage_probe tests/storage_probe.cpp)
    runtime_attach(protomate_simulation_storage_probe)
    target_include_directories(protomate_simulation_storage_probe PRIVATE ${PROJECT_SOURCE_DIR}/platform)
    add_executable(protomate_simulation_test_launcher tests/launcher.cpp)
    target_link_libraries(protomate_simulation_test_launcher PRIVATE protomate_simulation_session)
    add_simulation_pytest(storage test_storage.py)
    set_tests_properties(simulation.storage PROPERTIES TIMEOUT 125)
    add_simulation_pytest(lifecycle test_lifecycle.py)
    set_tests_properties(simulation.lifecycle PROPERTIES TIMEOUT 60)
    add_executable(protomate_simulation_launch_tests tests/launch_test.cpp)
    target_link_libraries(protomate_simulation_launch_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_session protomate_simulation_image)
    add_test(NAME simulation.launch COMMAND protomate_simulation_launch_tests)
    set_tests_properties(simulation.launch PROPERTIES TIMEOUT 10)
    add_simulation_pytest(packaging test_packaging.py)
    set_tests_properties(simulation.packaging PROPERTIES TIMEOUT 60)
    find_program(SIM_XVFB_RUN xvfb-run)
    if(SIM_XVFB_RUN)
        add_test(NAME simulation.opengl COMMAND ${SIM_XVFB_RUN} -a $<TARGET_FILE:protomate_simulation_gl_tests>)
        set_tests_properties(simulation.opengl PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;SDL_VIDEODRIVER=x11")
    endif()
    add_executable(protomate_simulation_tests tests/machine_test.cpp)
    target_link_libraries(protomate_simulation_tests PRIVATE protomate_simulation_gtest_main protomate_simulation_machine)
    add_test(NAME simulation.machine COMMAND protomate_simulation_tests)
    set_tests_properties(simulation.machine PROPERTIES TIMEOUT 15)
    add_simulation_pytest(hmi test_hmi.py)
    set_tests_properties(simulation.hmi PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    add_simulation_pytest(motion_api test_motion_api.py)
    set_tests_properties(simulation.motion_api PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    add_simulation_pytest(integration test_integration.py)
    set_tests_properties(simulation.integration PROPERTIES TIMEOUT 180)
endif()
