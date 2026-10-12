add_subdirectory(app)
add_subdirectory(motion)

add_library(proto_io STATIC)
add_library(proto::io ALIAS proto_io)

target_link_libraries(proto_io 
    PUBLIC
        proto::common
    PRIVATE
        proto::io::app
        proto::io::motion
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Instantiating the motors pulls in the C++ logging/units/runtime templates.
    # As with ICC, retain debug symbols and stepping without exceeding CM4 flash.
    target_compile_options(proto_io PRIVATE $<$<CONFIG:Debug>:-Og>)
endif()
