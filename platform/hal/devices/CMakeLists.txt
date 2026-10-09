add_subdirectory(factory)
add_subdirectory(impl)
add_subdirectory(itf)
add_subdirectory(util)

add_library(hal_devices STATIC)
add_library(hal::devices ALIAS hal_devices)

target_link_libraries(hal_devices 
    PUBLIC 
        hal::drivers
    PRIVATE
        hal::devices::factory
        hal::devices::impl
        hal::devices::itf
        hal::devices::util
)
