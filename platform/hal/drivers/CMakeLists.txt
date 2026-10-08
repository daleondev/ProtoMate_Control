add_library(hal_drivers STATIC)
add_library(hal::drivers ALIAS hal_drivers)

target_include_directories(hal_drivers
    PUBLIC
        ${PROJECT_SOURCE_DIR}/platform
)

target_link_libraries(hal_drivers
    PUBLIC
        platform
        project_compiler_settings
        pneumo::units
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_compile_definitions(hal_drivers PUBLIC HAL_PLATFORM_STM32)
    target_link_libraries(hal_drivers PUBLIC runtime::cxx_abi)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(hal_drivers PUBLIC HAL_PLATFORM_LINUX HAL_STEP_THREADX)
    target_link_libraries(hal_drivers PUBLIC threadx)
else()
    message(FATAL_ERROR "Unsupported HAL driver platform: ${CMAKE_SYSTEM_NAME}")
endif()

add_subdirectory(impl)
add_subdirectory(factory)
