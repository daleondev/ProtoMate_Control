add_library(proto_ui_app INTERFACE)
add_library(proto::ui::app ALIAS proto_ui_app)

target_sources(proto_ui_app
    INTERFACE
        Application.hpp
        Application.cpp 
)

target_link_libraries(proto_ui_app 
    INTERFACE
        runtime::threads
)
