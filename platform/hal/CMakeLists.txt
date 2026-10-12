add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(drivers)
add_subdirectory(devices)
add_subdirectory(board)
add_subdirectory(core)

add_library(hal STATIC)
add_library(platform::hal ALIAS hal)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(HAL_PLATFORM_DIRECTORY stm32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(HAL_PLATFORM_DIRECTORY linux)
else()
    message(FATAL_ERROR "Unsupported HAL platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_sources(hal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${HAL_PLATFORM_DIRECTORY}/hal.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal PRIVATE linux/startup.cpp linux/console.cpp)
endif()

target_link_libraries(hal
    PUBLIC
        hal::core
        platform
        project_compiler_settings
)

target_compile_definitions(hal PUBLIC
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Generic>:HAL_PLATFORM_STM32>
    $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Linux>:HAL_PLATFORM_LINUX>)
