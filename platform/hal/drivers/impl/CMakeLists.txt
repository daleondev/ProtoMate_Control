add_library(hal_drivers_impl INTERFACE)
add_library(hal::drivers::impl ALIAS hal_drivers_impl)

target_sources(hal_drivers_impl
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/StepGenerator.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StepGenerator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StepOutput.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/StepOutput.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal_drivers_impl INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/StepHardware.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/StepHardware.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/QuadratureEncoder.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/QuadratureEncoder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Spi.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Spi.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Uart.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Uart.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/InterCoreChannel.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/InterCoreChannel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rtc.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rtc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rng.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rng.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Gpio.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Gpio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Timer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Timer.cpp)
    if(STM32_CORE STREQUAL "CM7")
        target_sources(hal_drivers_impl INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Graphics.hpp ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Graphics.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/RgbOutput.hpp ${CMAKE_CURRENT_SOURCE_DIR}/stm32/RgbOutput.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/I2c.hpp ${CMAKE_CURRENT_SOURCE_DIR}/stm32/I2c.cpp)
    endif()
    return()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Background services use the runtime C++ ABI; the remaining Linux HAL
    # retains its native ABI and crosses through opaque service objects.
    add_library(hal_linux_services OBJECT linux/StepHardware.cpp linux/QuadratureEncoder.cpp)
    target_include_directories(hal_linux_services PRIVATE ${PROJECT_SOURCE_DIR}/platform)
    target_link_libraries(hal_linux_services PRIVATE runtime::cxx_abi project_compiler_settings)
    target_compile_definitions(hal_linux_services PRIVATE HAL_PLATFORM_LINUX HAL_STEP_THREADX)
    target_sources(hal_drivers_impl INTERFACE $<TARGET_OBJECTS:hal_linux_services>)

    target_sources(hal_drivers_impl
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Graphics.hpp ${CMAKE_CURRENT_SOURCE_DIR}/linux/Graphics.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/RgbOutput.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/RgbOutput.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Ethernet.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Gpio.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/PwmOutput.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/PwmOutput.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rng.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rtc.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Timer.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Timer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Uart.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Uart.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Spi.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Spi.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/SimulatedStepHardware.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/SimulatedStepHardware.cpp
    )

else()
    message(FATAL_ERROR "Unsupported HAL platform: ${CMAKE_SYSTEM_NAME}")
endif()
