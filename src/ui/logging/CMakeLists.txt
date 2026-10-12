add_library(proto_ui_logging INTERFACE)
add_library(proto::ui::logging ALIAS proto_ui_logging)
target_sources(proto_ui_logging INTERFACE Subscriber.hpp Subscriber.cpp)
