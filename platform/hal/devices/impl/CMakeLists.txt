add_library(hal_devices_impl INTERFACE)
add_library(hal::devices::impl ALIAS hal_devices_impl)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal_devices_impl INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Led.hpp ${CMAKE_CURRENT_SOURCE_DIR}/Led.cpp)
    return()
endif()

target_sources(hal_devices_impl
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Lan9253.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Lan9253.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Dm542tDriver.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Dm542tDriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209Bus.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209Bus.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209Driver.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209Driver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Button.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Button.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Led.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Led.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tmc2209.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IndexFeedback.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/IndexFeedback.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/QuadratureEncoderFeedback.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/QuadratureEncoderFeedback.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal_devices_impl
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Tmc2209Model.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/Tmc2209Model.cpp
    )
endif()
