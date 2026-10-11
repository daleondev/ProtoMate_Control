add_library(application_control_common STATIC AxisConversion.cpp ScaraKinematics.cpp MotionProfile.cpp Messages.hpp)
add_library(application::control_common ALIAS application_control_common)
target_include_directories(application_control_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${PROJECT_SOURCE_DIR}/platform)
target_link_libraries(application_control_common PUBLIC project_compiler_settings pneumo::pneumo application::p2p runtime::cxx_abi)

add_library(application_control_client STATIC ControllerClient.cpp Robot.cpp)
add_library(application::control_client ALIAS application_control_client)
target_link_libraries(application_control_client PUBLIC application::control_common)

add_library(application_control_io STATIC MotionController.cpp DriverControl.cpp StepperMotor.cpp MotionSequence.cpp SynchronizedSequence.cpp ControllerServer.cpp)
add_library(application::control_io ALIAS application_control_io)
target_link_libraries(application_control_io PUBLIC application::control_common runtime::runtime platform::hal)

# Compatibility aggregate for the present single-core executable and IO tests.
add_library(application_control INTERFACE)
add_library(application::control ALIAS application_control)
target_link_libraries(application_control INTERFACE application::control_client application::control_io)
foreach(target application_control_common application_control_client application_control_io)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
set_source_files_properties(MotionProfile.cpp MotionSequence.cpp SynchronizedSequence.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CONFIG:Release>:-O3>")
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # These translation units instantiate the messaging templates. Motion
    # execution keeps its normal Debug settings; -Og preserves useful stepping.
    set_source_files_properties(ControllerClient.cpp ControllerServer.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-Og>")
endif()
