add_subdirectory(gui)
add_subdirectory(app)
add_subdirectory(logging)
add_subdirectory(cli)
add_subdirectory(motion)

add_library(proto_ui STATIC)
add_library(proto::ui ALIAS proto_ui)

target_link_libraries(proto_ui 
    PUBLIC
        proto::common
    PRIVATE
        proto::ui::gui
        proto::ui::app
        proto::ui::logging
        proto::ui::cli
        proto::ui::motion
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # Motor commands and their serialized service/action endpoints must fit each
    # core's 1 MiB flash bank. Retain Debug symbols with size optimization.
    target_compile_options(proto_ui PRIVATE $<$<CONFIG:Debug>:-Os>)
    set_property(SOURCE main.cpp TARGET_DIRECTORY ${PROJECT_NAME} APPEND PROPERTY
        COMPILE_OPTIONS $<$<CONFIG:Debug>:-Os>)
endif()
