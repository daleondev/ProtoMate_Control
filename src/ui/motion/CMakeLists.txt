add_library(proto_ui_motion INTERFACE)
add_library(proto::ui::motion ALIAS proto_ui_motion)

target_sources(proto_ui_motion INTERFACE
    Client.hpp
    Client.cpp
)

target_link_libraries(proto_ui_motion INTERFACE runtime::runtime pneumo::coroutines)
