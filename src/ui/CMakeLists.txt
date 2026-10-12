add_subdirectory(app)
add_subdirectory(logging)
add_subdirectory(cli)

add_library(proto_ui STATIC)
add_library(proto::ui ALIAS proto_ui)

target_link_libraries(proto_ui 
    PUBLIC
        proto::common
    PRIVATE
        proto::ui::app
        proto::ui::logging
        proto::ui::cli
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Keep reflection/formatting templates compact while retaining Debug symbols.
    target_compile_options(proto_ui PRIVATE $<$<CONFIG:Debug>:-Og>)
endif()
