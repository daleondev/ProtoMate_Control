add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")

add_subdirectory(drivers)
add_subdirectory(devices)
add_subdirectory(board)
add_subdirectory(core)

add_library(hal STATIC)
add_library(platform::hal ALIAS hal)
target_sources(hal PRIVATE console.hpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(HAL_PLATFORM_DIRECTORY stm32)
    target_sources(hal PRIVATE stm32/FaultShutdown.cpp stm32/FaultShutdown.hpp stm32/TimerClock.hpp stm32/console.cpp stm32/UsbConsole.h)
    if(STM32_CORE STREQUAL "CM4")
        set_source_files_properties(stm32/FaultShutdown.cpp PROPERTIES COMPILE_OPTIONS
            "-O2;-include;${PROJECT_SOURCE_DIR}/platform/boot/giga/fast_unwind.h")
    endif()
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)
    # Native SDL/pthread boundary: never compile with the ThreadX C++ ABI.
    add_library(hal_linux_display STATIC linux/DisplayHost.cpp linux/DisplayHost.hpp)
    target_include_directories(hal_linux_display PUBLIC ${PROJECT_SOURCE_DIR}/platform)
    target_link_libraries(hal_linux_display PRIVATE PkgConfig::SDL2)
    target_link_libraries(hal_drivers PRIVATE hal_linux_display)
endif()
