add_library(hal_drivers_factory INTERFACE)
add_library(hal::drivers::factory ALIAS hal_drivers_factory)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal_drivers_factory INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/step.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/encoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/uart.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/intercore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/gpio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/timer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rtc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rng.cpp)
    return()
endif()

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
        ${CMAKE_CURRENT_SOURCE_DIR}/linux/pwm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/step.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/linux/step.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/encoder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/linux/encoder.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_drivers_factory
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/intercore.cpp
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
