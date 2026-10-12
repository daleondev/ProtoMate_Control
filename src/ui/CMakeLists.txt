add_subdirectory(app)

add_library(proto_ui STATIC)
add_library(proto::ui ALIAS proto_ui)

target_link_libraries(proto_ui 
    PUBLIC
        proto::common
    PRIVATE
        proto::ui::app
)
