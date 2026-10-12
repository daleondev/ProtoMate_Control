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
    # Catalog/logger templates also instantiate in the entrypoint. Optimize
    # those composition files for size to fit M7's 1 MiB flash bank.
    set_property(SOURCE app/Application.cpp logging/Sink.cpp APPEND PROPERTY
        COMPILE_OPTIONS $<$<CONFIG:Debug>:-Os>)
    set_property(SOURCE main.cpp TARGET_DIRECTORY ${PROJECT_NAME} APPEND PROPERTY
        COMPILE_OPTIONS $<$<CONFIG:Debug>:-Os>)
endif()
