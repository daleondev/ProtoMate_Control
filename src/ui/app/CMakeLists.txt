add_library(proto_ui_app INTERFACE)
add_library(proto::ui::app ALIAS proto_ui_app)

target_sources(proto_ui_app
    INTERFACE
        Application.hpp
        Application.cpp
        Services.hpp
        Services.cpp
        Operations.cpp
        services/Types.hpp
        services/Text.hpp
        GuiBackend.hpp
        GuiBackend.cpp
        SystemState.hpp
        SystemState.cpp
)

target_link_libraries(proto_ui_app 
    INTERFACE
        runtime::threads
)
if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # INTERFACE sources compile in proto_ui's directory, not this directory.
    # Set the property where those translation units are actually consumed.
    set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Services.cpp ${CMAKE_CURRENT_SOURCE_DIR}/Operations.cpp
        DIRECTORY ${PROJECT_SOURCE_DIR}/src/ui APPEND PROPERTY COMPILE_OPTIONS -mno-unaligned-access)
endif()
