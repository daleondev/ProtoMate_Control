add_subdirectory(app)

add_library(proto_io STATIC)
add_library(proto::io ALIAS proto_io)

target_link_libraries(proto_io 
    PUBLIC
        proto::common
    PRIVATE
        proto::io::app
)
