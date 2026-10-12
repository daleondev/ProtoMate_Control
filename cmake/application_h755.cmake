if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" OR NOT H755_CORE MATCHES "^CM[47]$")
    message(FATAL_ERROR "Use an H755 application preset")
endif()

set(HAL_BOARD nucleo_h755ziq)
set(RUNTIME_FILESYSTEM ON CACHE BOOL "Build FileX/LevelX filesystem support")
set(RUNTIME_STORAGE_REQUIRED OFF)
set(BUILD_TESTING OFF)
if(H755_CORE STREQUAL "CM4")
    set(THREADX_ARCH cortex_m4)
    set(application_role io)
    set(H755_FLASH_ORIGIN 0x08100000)
    set(H755_RAM_ORIGIN 0x30000000)
    set(H755_RAM_LENGTH 280K)
    set(H755_STACK_ORIGIN 0x30046000)
else()
    set(THREADX_ARCH cortex_m7)
    set(application_role ui)
    set(H755_FLASH_ORIGIN 0x08000000)
    set(H755_RAM_ORIGIN 0x24000000)
    set(H755_RAM_LENGTH 512K)
    set(H755_STACK_ORIGIN 0x2001E000)
endif()

add_library(project_compiler_settings INTERFACE)
target_compile_features(project_compiler_settings INTERFACE cxx_std_26)
set(THREADX_TOOLCHAIN gnu)
set(TX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/tx_user.h)
add_subdirectory(external/threadx)
target_compile_definitions(threadx PUBLIC TX_ENABLE_STACK_CHECKING)

if(RUNTIME_FILESYSTEM)
    set(FX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/fx_user.h)
    add_subdirectory(external/filex)
    include(cmake/patch_filex.cmake)
    runtime_patch_filex()
    target_link_libraries(filex PRIVATE project_compiler_settings)
    set(LX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/lx_user.h)
    add_subdirectory(external/levelx)
    include(cmake/patch_levelx.cmake)
    runtime_patch_levelx()
    target_link_libraries(levelx PRIVATE project_compiler_settings)
endif()

set(cube ${PROJECT_SOURCE_DIR}/external/CubeMX)
set(core ${cube}/${H755_CORE})
set(hal_sources ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src)
add_library(platform OBJECT
    ${core}/Startup/startup_stm32h755xx_${H755_CORE}.s
    ${core}/Src/main.c ${core}/Src/gpio.c ${core}/Src/tim.c
    ${core}/Src/stm32h7xx_hal_msp.c ${core}/Src/stm32h7xx_it.c
    ${cube}/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/initialize.cpp
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/sysmem.c
)
foreach(driver hal hal_cortex hal_gpio hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex
               hal_flash hal_flash_ex hal_hsem hal_tim hal_tim_ex hal_rtc hal_rtc_ex hal_rng hal_rng_ex hal_uart hal_uart_ex hal_dma hal_dma_ex)
    target_sources(platform PRIVATE ${hal_sources}/stm32h7xx_${driver}.c)
endforeach()
if(NOT RUNTIME_FILESYSTEM)
    target_sources(platform PRIVATE ${core}/Src/syscalls.c)
endif()
if(H755_CORE STREQUAL "CM7")
    target_sources(platform PRIVATE ${core}/Src/rtc.c ${core}/Src/rng.c ${core}/Src/usart.c)
endif()
set_source_files_properties(${core}/Src/main.c PROPERTIES
    COMPILE_DEFINITIONS "Error_Handler=CubeMX_Error_Handler")
set_source_files_properties(${core}/Src/stm32h7xx_it.c PROPERTIES
    COMPILE_DEFINITIONS "PendSV_Handler=CubeMX_PendSV_Handler;SysTick_Handler=CubeMX_SysTick_Handler;HardFault_Handler=CubeMX_HardFault_Handler;MemManage_Handler=CubeMX_MemManage_Handler;BusFault_Handler=CubeMX_BusFault_Handler;UsageFault_Handler=CubeMX_UsageFault_Handler")
set_source_files_properties(${core}/Src/syscalls.c PROPERTIES
    COMPILE_DEFINITIONS "_times=CubeMX_times")
target_include_directories(platform PUBLIC
    ${PROJECT_SOURCE_DIR}/platform
    ${PROJECT_SOURCE_DIR}/platform/runtime
    ${core}/Inc
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc/Legacy
    ${PROJECT_SOURCE_DIR}/external/cmsis-core/CMSIS/Core/Include
    ${PROJECT_SOURCE_DIR}/external/cmsis-device-h7/Include)
target_compile_definitions(platform PUBLIC
    STM32H755xx CORE_${H755_CORE} USE_HAL_DRIVER USE_PWR_DIRECT_SMPS_SUPPLY
    USER_VECT_TAB_ADDRESS HAL_PLATFORM_STM32
    $<$<CONFIG:Debug>:DEBUG>)
target_link_libraries(platform PUBLIC threadx project_compiler_settings)
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/initialize.cpp
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/sysmem.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

add_executable(${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${application_role}-controller)
target_link_libraries(${PROJECT_NAME} PRIVATE platform project_compiler_settings)
target_compile_options(${PROJECT_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U)
configure_file(${PROJECT_SOURCE_DIR}/platform/runtime/stm32/h755.ld.in
               ${CMAKE_CURRENT_BINARY_DIR}/h755.ld @ONLY)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/h755.ld)
target_link_options(${PROJECT_NAME} PRIVATE
    -T${CMAKE_CURRENT_BINARY_DIR}/h755.ld --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.map)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    VERBATIM)

include(${PROJECT_SOURCE_DIR}/cmake/memory_usage.cmake)
firmware_memory_usage(${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.map)

add_subdirectory(external/pneumo)
add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
add_subdirectory(src)
