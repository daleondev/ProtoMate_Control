set(THREADX_ARCH cortex_m7)
set(THREADX_TOOLCHAIN gnu)
set(TX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/tx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/threadx)
target_link_libraries(threadx PRIVATE project_compiler_settings)

set(FX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/fx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/filex)
include(${PROJECT_SOURCE_DIR}/cmake/patch_filex.cmake)
runtime_patch_filex()
target_link_libraries(filex PRIVATE project_compiler_settings)

set(LX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/lx_user.h)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/levelx)
target_link_libraries(levelx PRIVATE project_compiler_settings)
get_target_property(levelx_sources levelx SOURCES)
list(FILTER levelx_sources EXCLUDE REGEX "/(fx_nand_|fx_nor_flash_simulator|lx_nand_|lx_nor_flash_simulator)")
set_property(TARGET levelx PROPERTY SOURCES "${levelx_sources}")

# This setting changes ThreadX-visible structures and must be consistent for
# the kernel and every consumer of tx_api.h.
target_compile_definitions(threadx PUBLIC TX_ENABLE_STACK_CHECKING)

include(${PROJECT_SOURCE_DIR}/cmake/patch_sdcard.cmake)
runtime_patch_sdcard(runtime_sdcard_source)

add_library(platform OBJECT)

# Startup and ThreadX low-level objects must be present directly in the final
# link. A static archive is insufficient because their references appear only
# after GNU ld reaches the ThreadX archive.
target_sources(platform
    PRIVATE
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/main.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/eth.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/gpio.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/dma.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/quadspi.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/rng.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/rtc.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/sdmmc.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/tim.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/usart.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/stm32h7xx_it.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/stm32h7xx_hal_msp.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/stm32h7xx_hal_timebase_tim.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/sysmem.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/system_stm32h7xx.c
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/tx_initialize_low_level.S
        ${PROJECT_SOURCE_DIR}/external/CubeMX/startup_stm32h753xx.s
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_tim.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_tim_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_cortex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_eth.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_eth_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_fdcan.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rcc.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rcc_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_flash.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_flash_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_gpio.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_hsem.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_dma_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_mdma.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_pwr.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_pwr_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_qspi.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_i2c.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_i2c_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_exti.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rng.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rng_ex.c
        ${runtime_sdcard_source}
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_sd_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_ll_sdmmc.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rtc.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_rtc_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_usart.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_usart_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_uart.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_uart_ex.c
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-nucleo-bsp/stm32h7xx_nucleo.c  
)

# CubeMX owns these functions outside USER CODE sections. Rename only the
# generated definitions so project-owned implementations remain stable across
# regeneration. Calls inside main.c still reach CubeMX_Error_Handler(), whose
# preserved USER CODE body delegates to hal_error_handler().
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/main.c
    PROPERTIES
        COMPILE_DEFINITIONS "Error_Handler=CubeMX_Error_Handler;HAL_TIM_PeriodElapsedCallback=CubeMX_HAL_TIM_PeriodElapsedCallback"
)
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/external/CubeMX/Src/stm32h7xx_it.c
    PROPERTIES
        COMPILE_DEFINITIONS "EXTI3_IRQHandler=CubeMX_EXTI3_IRQHandler;EXTI4_IRQHandler=CubeMX_EXTI4_IRQHandler;EXTI15_10_IRQHandler=CubeMX_EXTI15_10_IRQHandler;EXTI9_5_IRQHandler=CubeMX_EXTI9_5_IRQHandler;DMA1_Stream0_IRQHandler=CubeMX_DMA1_Stream0_IRQHandler;DMA1_Stream1_IRQHandler=CubeMX_DMA1_Stream1_IRQHandler;DMA1_Stream2_IRQHandler=CubeMX_DMA1_Stream2_IRQHandler;DMA1_Stream3_IRQHandler=CubeMX_DMA1_Stream3_IRQHandler;TIM7_IRQHandler=CubeMX_TIM7_IRQHandler"
)
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-nucleo-bsp/stm32h7xx_nucleo.c
    PROPERTIES
        # Use the project console's ThreadX locking and UART error reporting.
        COMPILE_DEFINITIONS "__io_putchar=BSP_COM_putchar"
)

include(${PROJECT_SOURCE_DIR}/cmake/verify_cubemx.cmake)
verify_cubemx_generation()

target_include_directories(platform
    PRIVATE
        ${PROJECT_SOURCE_DIR}/platform
    PUBLIC
        # tx_api.h includes this as "tx_user.h". It must precede CubeMX/Inc,
        # which also contains CubeMX's generated tx_user.h.
        ${PROJECT_SOURCE_DIR}/platform/runtime
        ${PROJECT_SOURCE_DIR}/external/CubeMX/Inc
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Inc/Legacy
        ${PROJECT_SOURCE_DIR}/external/stm32h7xx-nucleo-bsp
        ${PROJECT_SOURCE_DIR}/external/cmsis-core/CMSIS/Core/Include
        ${PROJECT_SOURCE_DIR}/external/cmsis-device-h7/Include
)

target_compile_definitions(platform
    PRIVATE
        # Ethernet descriptors and raw frame buffers are linked into D2
        # SRAM2/SRAM3. SystemInit() uses this definition to enable all D2 SRAM
        # interfaces before the MPU, D-cache, and Ethernet DMA are started.
        DATA_IN_D2_SRAM
        # The HAL default is UINT32_MAX milliseconds, which turns a missing
        # or miswired SD data line into an apparent firmware hang.
        SDMMC_SWDATATIMEOUT=5000U
    PUBLIC
        USE_PWR_LDO_SUPPLY
        USE_HAL_DRIVER
        STM32H753xx
        $<$<CONFIG:Debug>:DEBUG>
)

target_link_libraries(platform
    PUBLIC
        filex
        levelx
        threadx
)

target_compile_features(platform
    PUBLIC
        c_std_11
)
