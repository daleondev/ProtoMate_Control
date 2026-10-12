add_library(hal_devices_factory INTERFACE)
add_library(hal::devices::factory ALIAS hal_devices_factory)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    return()
endif()

target_sources(hal_devices_factory
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/tmc2209.hpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_devices_factory
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/tmc2209.cpp
    )
endif()
