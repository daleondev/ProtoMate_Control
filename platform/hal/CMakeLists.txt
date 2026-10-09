add_subdirectory(drivers)
add_subdirectory(devices)
add_subdirectory(board)

add_library(hal STATIC)
add_library(platform::hal ALIAS hal)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(HAL_PLATFORM_DIRECTORY stm32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(HAL_PLATFORM_DIRECTORY linux)
else()
    message(FATAL_ERROR "Unsupported HAL platform: ${CMAKE_SYSTEM_NAME}")
endif()

target_sources(hal
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/hal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/${HAL_PLATFORM_DIRECTORY}/hal.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(hal
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/linux/console.cpp
    )
endif()

target_link_libraries(hal
    PUBLIC
        hal::drivers
        platform
        project_compiler_settings
)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(hal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stm32/FaultShutdown.cpp)
    if(HAL_HARDWARE_TMC_STANDALONE_TEST)
        target_compile_definitions(hal PRIVATE HAL_HARDWARE_TMC_STANDALONE_TEST)
    endif()
    if(HAL_HARDWARE_SPI_TEST OR HAL_HARDWARE_ETHERCAT_SLAVE)
        target_compile_definitions(hal PRIVATE HAL_HARDWARE_SPI_TEST)
    endif()
    target_compile_definitions(hal
        PUBLIC
            HAL_PLATFORM_STM32
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(hal
        PUBLIC
            HAL_PLATFORM_LINUX
    )
endif()
