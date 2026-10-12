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
