add_library(application_control STATIC)
add_library(application::control ALIAS application_control)

target_sources(application_control
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thread.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionController.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Robot.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Robot.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StepperMotor.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StepperMotor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionProfile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionProfile.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionSequence.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotionSequence.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AxisConversion.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/AxisConversion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ScaraKinematics.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ScaraKinematics.cpp
)

target_include_directories(application_control
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(application_control
    PUBLIC
        project_compiler_settings
        pneumo::pneumo
        runtime::runtime
        platform::hal
)

target_compile_options(application_control
    PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# Curve inversion runs in the DMA refill interrupt; speed matters more than
# code size here. Keep normal debug stepping and retain IEEE floating-point rules.
set_source_files_properties(MotionProfile.cpp MotionSequence.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CONFIG:Release>:-O3>")
