add_subdirectory(app)
add_subdirectory(diagnostics)
add_subdirectory(logging)
add_subdirectory(motion)

add_library(proto_io STATIC)
add_library(proto::io ALIAS proto_io)

target_link_libraries(proto_io 
    PUBLIC
        proto::common
    PRIVATE
        proto::io::app
        proto::io::diagnostics
        proto::io::logging
        proto::io::motion
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Motor commands and their serialized service/action endpoints must fit each
    # core's 1 MiB flash bank. Retain Debug symbols with size optimization.
    target_compile_options(proto_io PRIVATE $<$<CONFIG:Debug>:-Os>)
    set_property(SOURCE main.cpp TARGET_DIRECTORY ${PROJECT_NAME} APPEND PROPERTY
        COMPILE_OPTIONS $<$<CONFIG:Debug>:-Os>)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # These cold-side entry points instantiate telemetry/ICC serializers. M4 has
    # no QSPI instruction cache: retain inlining instead of -Os helper chains.
    set_property(SOURCE app/Application.cpp motion/Server.cpp motion/ServerPrograms.cpp
        motion/ControllerAdmission.cpp motion/ControllerSetup.cpp APPEND PROPERTY COMPILE_OPTIONS -O2)
endif()
