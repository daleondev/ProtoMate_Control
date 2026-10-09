add_library(hal_devices_util INTERFACE)
add_library(hal::devices::util ALIAS hal_devices_util)

target_sources(hal_devices_util
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/DriverFaultMonitor.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DriverFaultMonitor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/DriverMutex.hpp
)