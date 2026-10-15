include(${PROJECT_SOURCE_DIR}/cmake/touchgfx.cmake)
add_library(proto_ui_gui STATIC
    Controller.hpp Controller.cpp util/FrameTimer.hpp
    frontend/Theme.hpp frontend/Model.hpp
    frontend/Frontend.hpp frontend/Frontend.cpp
    frontend/Components.hpp frontend/Components.cpp
    integration/OS.hpp integration/OS.cpp
    integration/GraphicsDMA.hpp integration/GraphicsDMA.cpp)
add_library(proto::ui::gui ALIAS proto_ui_gui)
target_include_directories(proto_ui_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(proto_ui_gui PUBLIC runtime::runtime platform::hal PRIVATE touchgfx::support pneumo::logging)
add_dependencies(proto_ui_gui gui-assets)
# Rendering must be optimized even in Debug; preserve symbols.
target_compile_options(proto_ui_gui PRIVATE -O2)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/integration/Host.cpp)
endif()

if(STM32_CORE STREQUAL "CM7")
    target_compile_options(proto_ui_gui PRIVATE -fno-rtti)
endif()
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(gui_port_tests tests/graphics_test.cpp)
    target_link_libraries(gui_port_tests PRIVATE proto::ui::gui GTest::gtest_main touchgfx::engine)
    runtime_attach(gui_port_tests)
    add_test(NAME gui.graphics COMMAND gui_port_tests)
    set_tests_properties(gui.graphics PROPERTIES TIMEOUT 10)
    add_library(gui_test_native OBJECT tests/native_host.cpp)
    target_include_directories(gui_test_native PRIVATE ${PROJECT_SOURCE_DIR}/platform)
    target_link_libraries(gui_test_native PRIVATE PkgConfig::SDL2)
    add_executable(gui_integration_tests tests/controller_test.cpp $<TARGET_OBJECTS:gui_test_native>)
    target_link_libraries(gui_integration_tests PRIVATE proto::ui::gui)
    target_compile_definitions(gui_integration_tests PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=32768U)
    runtime_attach(gui_integration_tests)
    add_test(NAME gui.integration COMMAND gui_integration_tests)
    add_test(NAME gui.presentation_timeout COMMAND gui_integration_tests)
    set_tests_properties(gui.presentation_timeout PROPERTIES TIMEOUT 10 ENVIRONMENT "SDL_VIDEODRIVER=dummy;GUI_TEST_STALL=1")
    set_tests_properties(gui.integration PROPERTIES TIMEOUT 20 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    find_program(XVFB_RUN xvfb-run)
    if(XVFB_RUN)
        add_test(NAME gui.x11 COMMAND ${XVFB_RUN} -a -s "-screen 0 1600x900x24"
            ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=x11 $<TARGET_FILE:gui_integration_tests>)
        # Exercise the real SDL/Mesa shared-library ABI, independently of the
        # dummy renderer. Isolate its captures from the parallel dummy test.
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/x11)
        set_tests_properties(gui.x11 PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/x11)
    endif()
endif()
