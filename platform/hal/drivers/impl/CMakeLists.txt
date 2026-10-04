target_sources(hal_drivers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/StepGenerator.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")

    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/PwmOutput.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/StepHardware.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/QuadratureEncoder.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/stm32/Timer.cpp
    )

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Only the service thread uses the runtime C++ ABI; the Linux HAL retains
    # its native ABI and crosses this boundary through an opaque service object.
    add_library(hal_linux_step_service STATIC linux/StepService.cpp)
    target_link_libraries(hal_linux_step_service PRIVATE runtime::cxx_abi project_compiler_settings)
    target_compile_definitions(hal_linux_step_service PRIVATE HAL_PLATFORM_LINUX HAL_STEP_THREADX)
    target_link_libraries(hal_drivers PRIVATE hal_linux_step_service)


    target_sources(hal_drivers
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Ethernet.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Gpio.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/PwmOutput.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/QuadratureEncoder.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rng.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Rtc.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Timer.cpp
    )

else()
    message(FATAL_ERROR "Unsupported HAL platform: ${CMAKE_SYSTEM_NAME}")
endif()
