if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/timer.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/timer.cpp
    )
else()
    message(FATAL_ERROR "Unsupported HAL driver platform: ${CMAKE_SYSTEM_NAME}")
endif()
