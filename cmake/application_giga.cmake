if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" OR NOT STM32_CORE MATCHES "^CM[47]$")
    message(FATAL_ERROR "Use an GIGA application preset")
endif()

set(HAL_BOARD arduino_giga)
set(RUNTIME_FILESYSTEM ON CACHE BOOL "Build FileX/LevelX filesystem support")
set(RUNTIME_STORAGE_REQUIRED OFF)
set(BUILD_TESTING OFF)
if(STM32_CORE STREQUAL "CM4")
    set(THREADX_ARCH cortex_m4)
    set(application_role io)
    set(GIGA_FLASH_ORIGIN 0x90100000)
    set(GIGA_FLASH_LENGTH 3M)
    set(GIGA_CODE_ORIGIN 0x90100000)
    set(GIGA_CODE_LENGTH 3M)
    if(GIGA_M4_RAM_DEBUG)
        set(GIGA_CODE_ORIGIN 0xC0400000)
        set(GIGA_CODE_LENGTH 2M)
    endif()
    set(GIGA_VECTOR_ORIGIN 0x10000000)
    set(GIGA_FAST_ORIGIN 0x10000800)
    set(GIGA_FAST_LENGTH 126K)
    set(GIGA_TCM_ORIGIN 0x30020000)
    set(GIGA_TCM_LENGTH 0)
    set(GIGA_RAM_ORIGIN 0x30020000)
    set(GIGA_RAM_LENGTH 152K)
    set(GIGA_STACK_ORIGIN 0x30046000)
    set(GIGA_HEAP_REGION HEAP)
    include(${PROJECT_SOURCE_DIR}/cmake/giga_fast_code.cmake)
else()
    set(THREADX_ARCH cortex_m7)
    set(application_role ui)
    set(GIGA_FLASH_ORIGIN 0x90400000)
    set(GIGA_FLASH_LENGTH 4M)
    set(GIGA_CODE_ORIGIN 0x90400000)
    set(GIGA_CODE_LENGTH 4M)
    set(GIGA_VECTOR_ORIGIN 0x20000000)
    set(GIGA_FAST_ORIGIN 0x00000000)
    set(GIGA_FAST_LENGTH 64K)
    set(GIGA_TCM_ORIGIN 0x20000800)
    set(GIGA_TCM_LENGTH 110K)
    set(GIGA_RAM_ORIGIN 0x24000000)
    set(GIGA_RAM_LENGTH 256K)
    set(GIGA_STACK_ORIGIN 0x2001E000)
    set(GIGA_HEAP_REGION RAM)
    set(GIGA_FAST_OBJECTS "")

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
set(core ${cube}/${STM32_CORE})
set(hal_sources ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src)
add_library(platform OBJECT
    ${core}/Startup/startup_stm32h747xx_${STM32_CORE}.s
    ${core}/Src/main.c ${core}/Src/gpio.c ${core}/Src/tim.c
    ${core}/Src/stm32h7xx_hal_msp.c ${core}/Src/stm32h7xx_it.c
    ${cube}/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/initialize.cpp
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/sysmem.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/application.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/entry.S
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/mpu.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/clocks.c
)
foreach(driver hal hal_cortex hal_gpio hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex
               hal_flash hal_flash_ex hal_hsem hal_tim hal_tim_ex hal_rtc hal_rtc_ex hal_rng hal_rng_ex hal_uart hal_uart_ex hal_dma hal_dma_ex hal_spi hal_spi_ex)
    target_sources(platform PRIVATE ${hal_sources}/stm32h7xx_${driver}.c)
endforeach()
if(NOT RUNTIME_FILESYSTEM)
    target_sources(platform PRIVATE ${core}/Src/syscalls.c)
endif()
if(STM32_CORE STREQUAL "CM7")
    target_sources(platform PRIVATE ${core}/Src/rtc.c ${core}/Src/rng.c
        ${core}/Src/dma2d.c ${core}/Src/ltdc.c ${core}/Src/i2c.c
        ${core}/Src/usb_device.c ${core}/Src/usbd_conf.c ${core}/Src/usbd_desc.c ${core}/Src/usbd_cdc_if.c)
    foreach(driver hal_pcd hal_pcd_ex ll_usb hal_ltdc hal_ltdc_ex hal_dma2d hal_i2c hal_i2c_ex)
        target_sources(platform PRIVATE ${hal_sources}/stm32h7xx_${driver}.c)
    endforeach()
    set(usb ${PROJECT_SOURCE_DIR}/external/stm32-usb-device)
    target_sources(platform PRIVATE ${usb}/Core/Src/usbd_core.c ${usb}/Core/Src/usbd_ctlreq.c
        ${usb}/Core/Src/usbd_ioreq.c ${usb}/Class/CDC/Src/usbd_cdc.c)
    target_include_directories(platform PUBLIC ${usb}/Core/Inc ${usb}/Class/CDC/Inc)
else()
    target_sources(platform PRIVATE ${core}/Src/dma.c ${core}/Src/usart.c ${core}/Src/spi.c)
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
    ${PROJECT_SOURCE_DIR}/platform/boot/giga
    ${core}/Inc
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc/Legacy
    ${PROJECT_SOURCE_DIR}/external/cmsis-core/CMSIS/Core/Include
    ${PROJECT_SOURCE_DIR}/external/cmsis-device-h7/Include)
target_compile_definitions(platform PUBLIC
    STM32H747xx CORE_${STM32_CORE} USE_HAL_DRIVER USE_PWR_LDO_SUPPLY
    HAL_RTC_MODULE_ENABLED= HAL_RNG_MODULE_ENABLED= HAL_UART_MODULE_ENABLED= HAL_SPI_MODULE_ENABLED=
    HAL_PLATFORM_STM32 GIGA_APPLICATION
    $<$<CONFIG:Debug>:DEBUG>)
target_link_libraries(platform PUBLIC threadx project_compiler_settings)
target_compile_options(platform PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Os>")
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/initialize.cpp
    ${PROJECT_SOURCE_DIR}/platform/runtime/stm32/sysmem.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/application.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/entry.S
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/mpu.c
    ${PROJECT_SOURCE_DIR}/platform/boot/giga/clocks.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

add_executable(${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${application_role}-controller)
target_link_libraries(${PROJECT_NAME} PRIVATE platform project_compiler_settings)
target_compile_options(${PROJECT_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U)
if(STM32_CORE STREQUAL "CM4" AND GIGA_M4_RAM_DEBUG)
    set(GIGA_CODE_MEMORY "CODE (rx) : ORIGIN = 0xC0400000, LENGTH = 2M")
    set(GIGA_CODE_ALIAS "")
else()
    set(GIGA_CODE_MEMORY "")
    set(GIGA_CODE_ALIAS "REGION_ALIAS(\"CODE\", FLASH);")
endif()
string(REPLACE ".text* .rodata*" ".ARM.extab*" GIGA_FAST_EXTAB "${GIGA_FAST_OBJECTS}")
string(REPLACE ".text* .rodata*" ".ARM.exidx*" GIGA_FAST_EXIDX "${GIGA_FAST_OBJECTS}")
string(REPLACE ".text." ".ARM.extab.text." GIGA_FAST_EXTAB "${GIGA_FAST_EXTAB}")
string(REPLACE ".text." ".ARM.exidx.text." GIGA_FAST_EXIDX "${GIGA_FAST_EXIDX}")
configure_file(${PROJECT_SOURCE_DIR}/platform/boot/giga/application.ld.in
               ${CMAKE_CURRENT_BINARY_DIR}/giga.ld @ONLY)
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/giga.ld)
target_link_options(${PROJECT_NAME} PRIVATE
    -T${CMAKE_CURRENT_BINARY_DIR}/giga.ld --specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage
    -Wl,--wrap=HAL_RCCEx_PeriphCLKConfig
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.map)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.bin
    COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.debug
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    VERBATIM)

include(${PROJECT_SOURCE_DIR}/cmake/memory_usage.cmake)
firmware_memory_usage(${PROJECT_NAME} ${CMAKE_CURRENT_BINARY_DIR}/${application_role}-controller.map)
if(STM32_CORE STREQUAL "CM4" AND NOT GIGA_STEP_BENCH)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(ram-audit ALL
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/giga_ram_audit.py
            $<TARGET_FILE:${PROJECT_NAME}> --report ${PROJECT_BINARY_DIR}/ram-audit.json
        DEPENDS ${PROJECT_NAME} VERBATIM)
endif()

add_subdirectory(external/pneumo)
add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
add_subdirectory(src)
include(${PROJECT_SOURCE_DIR}/cmake/bootstrap_giga.cmake)

if(STM32_CORE STREQUAL "CM4")
    set(fast_directories Application)
    foreach(owner proto_io proto_common hal hal_drivers hal_devices hal_devices_util runtime_synchronization runtime_libstdcxx runtime_threadx)
        if(TARGET ${owner})
            list(APPEND fast_directories ${owner})
        endif()
    endforeach()
    foreach(source IN LISTS giga_fast_sources)
        set(fast_opt -Os)
        if(source MATCHES "(StepGenerator|StepHardware)\\.cpp$")
            set(fast_opt -O2)
        endif()
        set_source_files_properties(${PROJECT_SOURCE_DIR}/${source}
            TARGET_DIRECTORY ${fast_directories}
            PROPERTIES COMPILE_OPTIONS "${fast_opt};-include;${PROJECT_SOURCE_DIR}/platform/boot/giga/fast_unwind.h")
    endforeach()
    target_compile_options(threadx PRIVATE -Os)
endif()

target_include_directories(runtime_threadx PRIVATE $<TARGET_PROPERTY:platform,INCLUDE_DIRECTORIES>)
target_compile_definitions(runtime_threadx PRIVATE $<TARGET_PROPERTY:platform,COMPILE_DEFINITIONS>)
