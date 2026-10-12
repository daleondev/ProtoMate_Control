add_subdirectory(factory)
add_subdirectory(impl)

add_library(hal_drivers STATIC)
add_library(hal::drivers ALIAS hal_drivers)

target_include_directories(hal_drivers
    PUBLIC
        ${PROJECT_SOURCE_DIR}/platform
)

target_link_libraries(hal_drivers
    PUBLIC
        platform
        platform::communication
        project_compiler_settings
        pneumo::units
    PRIVATE
        hal::drivers::factory
        hal::drivers::impl
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_compile_definitions(hal_drivers PUBLIC HAL_PLATFORM_STM32)
    target_link_libraries(hal_drivers PUBLIC runtime::cxx_abi)
    # DMA refill and arming have real-time deadlines, including in Debug.
    # Keep symbols, but optimize these two scheduling translation units on
    # the M4 as well as the M7; unoptimized STL access exceeds the arming budget.
    set_source_files_properties(impl/StepGenerator.cpp impl/stm32/StepHardware.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(hal_drivers PUBLIC HAL_PLATFORM_LINUX HAL_STEP_THREADX)
    target_link_libraries(hal_drivers PUBLIC threadx)
else()
    message(FATAL_ERROR "Unsupported HAL driver platform: ${CMAKE_SYSTEM_NAME}")
endif()

