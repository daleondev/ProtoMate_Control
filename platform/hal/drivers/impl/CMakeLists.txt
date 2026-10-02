if(CMAKE_SYSTEM_NAME STREQUAL "Generic")

    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Timer.cpp
    )

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Timer.cpp
    )

else()
    message(FATAL_ERROR "Unsupported HAL platform: ${CMAKE_SYSTEM_NAME}")
endif()
