add_subdirectory(linux)

add_library(proto_ui_cli INTERFACE)
add_library(proto::ui::cli ALIAS proto_ui_cli)

target_sources(proto_ui_cli INTERFACE
    Console.hpp 
    Console.cpp
    Parser.hpp 
    Parser.cpp
    filesystem.hpp 
    filesystem.cpp
    utilities.hpp 
    utilities.cpp
    display.hpp
    display.cpp
    state.hpp
    state.cpp
    motor.hpp 
    motor.cpp
)

target_link_libraries(proto_ui_cli INTERFACE
    pneumo::meta 
    runtime::runtime 
    platform::hal 
    proto::ui::cli::linux
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
