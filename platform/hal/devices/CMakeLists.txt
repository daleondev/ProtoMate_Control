target_sources(hal_drivers
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Lan9253.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/detail/DriverFaultMonitor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Dm542tDriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209Bus.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209Driver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Button.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Led.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/IndexFeedback.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/QuadratureEncoderFeedback.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_drivers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/impl/linux/Tmc2209Uart.cpp)
endif()
