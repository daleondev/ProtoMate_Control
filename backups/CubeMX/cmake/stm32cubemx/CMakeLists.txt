cmake_minimum_required(VERSION 3.22)
# Enable CMake support for ASM and C languages
enable_language(C ASM)
# STM32CubeMX generated symbols (macros)
set(MX_Defines_Syms 
	USE_PWR_LDO_SUPPLY 
	TX_INCLUDE_USER_DEFINE_FILE 
	FX_INCLUDE_USER_DEFINE_FILE 
	LX_INCLUDE_USER_DEFINE_FILE 
	USE_HAL_DRIVER 
	STM32H753xx
    $<$<CONFIG:Debug>:DEBUG>
)

# STM32CubeMX generated include paths
set(MX_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Inc
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Inc
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H7xx_Nucleo
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/CMSIS/Device/ST/STM32H7xx/Include
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/CMSIS/Include
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/inc/
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/ports/generic/inc/
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/inc/
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/inc/
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/inc/
)

# STM32CubeMX generated application sources
set(MX_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/tx_initialize_low_level.S
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/app_threadx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/app_azure_rtos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/app_filex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/lx_stm32_nor_custom_driver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/fx_stm32_sd_driver_glue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/eth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/quadspi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/rng.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/sdmmc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/usart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/stm32h7xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/stm32h7xx_hal_msp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/stm32h7xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../startup_stm32h753xx.s
)

# STM32 HAL/LL Drivers
set(STM32_Drivers_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/system_stm32h7xx.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_eth_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hsem.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_qspi.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_delayblock.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rng.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rng_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_sdmmc.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sd.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_sd_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mmc.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mmc_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_usart.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_usart_ex.c
    /home/david/STM32Cube/Repository/STM32Cube_FW_H7_V1.13.0/Drivers/BSP/STM32H7xx_Nucleo/stm32h7xx_nucleo.c
)

# Drivers Midllewares


set(Interfaces_Src
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/drivers/fx_stm32_levelx_nor_driver.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/drivers/fx_stm32_sd_driver.c
)
set(LevelX_Src
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_block_reclaim.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_close.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_defragment.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_driver_block_erase.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_driver_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_driver_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_extended_cache_enable.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_logical_sector_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_next_block_to_erase_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_open.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_partial_defragment.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_physical_sector_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_sector_mapping_cache_invalidate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_sector_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_sector_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_sector_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/levelx/common/src/lx_nor_flash_system_error.c
)
set(FileX_Src
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_attributes_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_attributes_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_default_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_default_get_copy.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_default_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_entry_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_entry_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_exFAT_entry_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_exFAT_entry_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_exFAT_free_search.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_exFAT_unicode_entry_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_first_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_first_full_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_free_search.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_information_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_local_path_clear.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_local_path_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_local_path_get_copy.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_local_path_restore.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_local_path_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_long_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_long_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_name_extract.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_name_test.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_next_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_next_full_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_search.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_short_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_directory_short_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_add_bitmap_log.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_add_checksum_log.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_add_dir_log.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_add_FAT_log.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_apply_logs.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_calculate_checksum.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_cleanup_FAT_chain.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_create_log_file.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_enable.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_read_directory_sector.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_read_FAT.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_read_log_file.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_recover.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_reset_log_file.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_set_FAT_chain.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_transaction_end.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_transaction_fail.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_transaction_start.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_fault_tolerant_write_log_file.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_attributes_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_attributes_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_best_effort_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_close.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_date_time_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_best_effort_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_relative_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_truncate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_extended_truncate_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_open.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_relative_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_truncate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_truncate_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_file_write_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_abort.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_boot_info_extract.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_cache_invalidate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_check_FAT_chain_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_check_lost_cluster_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_close.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_close_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_exFAT_format.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_extended_space_available.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_format.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_format_oem_name_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_format_type_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_format_volume_id_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_open.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_open_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_space_available.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_volume_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_volume_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_volume_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_media_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_partition_offset_calculate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_date_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_date_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_time_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_time_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_system_timer_entry.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_directory_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_directory_entry_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_directory_entry_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_directory_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_directory_search.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_file_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_file_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_length_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_length_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_short_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_unicode_short_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_16_unsigned_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_16_unsigned_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_32_unsigned_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_32_unsigned_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_64_unsigned_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_64_unsigned_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_absolute_path_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_allocate_new_cluster.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_cache_prepare.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_cache_update.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_free_cluster_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_bitmap_start_sector_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_cluster_free.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_cluster_state_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_cluster_state_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_geometry_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_name_hash_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_size_calculate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_system_area_checksum_verify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_system_area_checksum_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_system_area_format.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_system_sector_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_unicode_name_hash_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_exFAT_upcase_table.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_FAT_entry_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_FAT_entry_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_FAT_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_FAT_map_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_FAT_sector_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_logical_sector_cache_entry_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_logical_sector_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_logical_sector_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_logical_sector_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_memory_copy.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_memory_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_string_length_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fx_utility_token_length_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_attributes_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_attributes_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_default_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_default_get_copy.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_default_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_first_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_first_full_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_information_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_local_path_clear.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_local_path_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_local_path_get_copy.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_local_path_restore.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_local_path_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_long_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_long_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_name_test.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_next_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_next_full_entry_find.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_short_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_directory_short_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_fault_tolerant_enable.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_attributes_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_attributes_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_best_effort_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_close.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_date_time_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_best_effort_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_relative_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_truncate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_extended_truncate_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_open.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_relative_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_seek.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_truncate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_truncate_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_file_write_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_abort.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_cache_invalidate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_close.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_close_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_exFAT_format.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_extended_space_available.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_format.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_open.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_open_notify_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_read.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_space_available.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_volume_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_volume_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_volume_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_media_write.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_system_date_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_system_date_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_system_time_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_system_time_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_directory_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_directory_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_file_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_file_rename.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_name_get_extended.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_short_name_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/filex/common/src/fxe_unicode_short_name_get_extended.c
)
set(ThreadX_Src
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_context_restore.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_context_save.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_interrupt_control.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_interrupt_disable.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_interrupt_restore.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_schedule.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_stack_build.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_thread_system_return.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/ports/cortex_m7/gnu/src/tx_timer_interrupt.S
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_initialize_high_level.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_initialize_kernel_enter.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_initialize_kernel_setup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_pool_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_block_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_pool_search.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_byte_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_event_flags_set_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_priority_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_mutex_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_front_send.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_receive.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_send.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_queue_send_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_ceiling_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_cleanup.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_semaphore_put_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_entry_exit_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_identify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_preemption_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_priority_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_relinquish.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_reset.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_resume.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_shell_entry.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_sleep.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_stack_analyze.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_stack_error_handler.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_stack_error_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_suspend.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_system_preempt_check.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_system_resume.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_system_suspend.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_terminate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_time_slice.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_time_slice_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_timeout.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_thread_wait_abort.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_time_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_time_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_pool_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_pool_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_pool_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_pool_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_block_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_allocate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_pool_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_pool_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_pool_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_pool_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_byte_release.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_set.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_event_flags_set_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_mutex_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_flush.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_front_send.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_receive.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_send.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_queue_send_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_ceiling_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_prioritize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_put.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_semaphore_put_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_entry_exit_notify.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_preemption_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_priority_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_relinquish.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_reset.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_resume.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_suspend.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_terminate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_time_slice_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_thread_wait_abort.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_activate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_deactivate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_expiration_process.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_info_get.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_initialize.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_system_activate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_system_deactivate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/tx_timer_thread_entry.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_activate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_change.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_create.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_deactivate.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_delete.c
    /home/david/STM32Cube/Repository//Packs/STMicroelectronics/X-CUBE-AZRTOS-H7/3.5.0/Middlewares/ST/threadx/common/src/txe_timer_info_get.c
)

# Link directories setup
set(MX_LINK_DIRS

)
# Project static libraries
set(MX_LINK_LIBS 
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
    Interfaces	LevelX	FileX	ThreadX	
)
# Interface library for includes and symbols
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

# Create STM32_Drivers static library
add_library(STM32_Drivers OBJECT)
target_sources(STM32_Drivers PRIVATE ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)


# Create Interfaces static library
add_library(Interfaces OBJECT)
target_sources(Interfaces PRIVATE ${Interfaces_Src})
target_link_libraries(Interfaces PUBLIC stm32cubemx)

# Create LevelX static library
add_library(LevelX OBJECT)
target_sources(LevelX PRIVATE ${LevelX_Src})
target_link_libraries(LevelX PUBLIC stm32cubemx)

# Create FileX static library
add_library(FileX OBJECT)
target_sources(FileX PRIVATE ${FileX_Src})
target_link_libraries(FileX PUBLIC stm32cubemx)

# Create ThreadX static library
add_library(ThreadX OBJECT)
target_sources(ThreadX PRIVATE ${ThreadX_Src})
target_link_libraries(ThreadX PUBLIC stm32cubemx)

# Add STM32CubeMX generated application sources to the project
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MX_Application_Src})

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MX_LINK_DIRS})

# Add libraries to the project
target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})

# Add the map file to the list of files to be removed with 'clean' target
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
