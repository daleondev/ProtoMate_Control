add_library(hal_drivers_factory INTERFACE)
add_library(hal::drivers::factory ALIAS hal_drivers_factory)

target_sources(hal_drivers_factory
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethernet.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/gpio.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rng.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rtc.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spi.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/timer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/uart.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pwm.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pwm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/step.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/step.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/encoder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/encoder.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    include(${PROJECT_SOURCE_DIR}/cmake/hal_serial_instances.cmake)
    hal_generate_serial_instances(${CMAKE_CURRENT_BINARY_DIR}/generated
                                  ${PROJECT_SOURCE_DIR}/external/CubeMX/Inc)

    target_include_directories(hal_drivers_factory 
        INTERFACE 
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_sources(hal_drivers_factory
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/timer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/spi.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/uart.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_drivers_factory
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/timer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/spi.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/uart.cpp
    )
else()
    message(FATAL_ERROR "Unsupported HAL driver platform: ${CMAKE_SYSTEM_NAME}")
endif()