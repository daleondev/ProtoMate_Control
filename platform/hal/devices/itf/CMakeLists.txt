add_library(hal_devices_itf INTERFACE)
add_library(hal::devices::itf ALIAS hal_devices_itf)

target_sources(hal_devices_itf
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/IDisplay.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ITouch.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MotorTypes.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IButton.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IConfigurableStepperDriver.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ILed.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IMotorFeedback.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IStepperDriver.hpp
)