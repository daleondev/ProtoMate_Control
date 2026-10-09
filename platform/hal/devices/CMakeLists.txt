add_library(hal_devices STATIC)
add_library(hal::devices ALIAS hal_devices)
target_link_libraries(hal_devices PUBLIC hal::drivers)

target_sources(hal_devices
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Lan9253.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/util/DriverFaultMonitor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Dm542tDriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209Bus.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209Driver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Button.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Led.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/Tmc2209.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/IndexFeedback.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/QuadratureEncoderFeedback.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal_devices PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/factory/stm32/tmc2209.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_devices PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/factory/linux/tmc2209.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/impl/linux/Tmc2209Model.cpp)
endif()
