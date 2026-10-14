# Internal recovery images and RAM-only external-memory initializer. They do
# not link ThreadX, C++ runtime, application devices or QSPI-resident code.
function(giga_boot_image target helper)
    set(boot ${PROJECT_SOURCE_DIR}/platform/boot/giga)
    if(STM32_CORE STREQUAL "CM4")
        set(BOOT_CODE_ORIGIN 0x08100000)
        set(BOOT_RAM_ORIGIN 0x30030000)
        set(BOOT_STACK_ORIGIN 0x30046000)
    else()
        set(BOOT_CODE_ORIGIN 0x08000000)
        set(BOOT_RAM_ORIGIN 0x2001C000)
        set(BOOT_STACK_ORIGIN 0x2001E000)
    endif()
    set(BOOT_CODE_LENGTH 128K)
    if(helper)
        set(BOOT_CODE_ORIGIN 0x24000000)
        set(BOOT_CODE_LENGTH 256K)
    endif()
    configure_file(${boot}/bootstrap.ld.in ${CMAKE_CURRENT_BINARY_DIR}/${target}.ld @ONLY)
    add_executable(${target} ${boot}/bootstrap.c ${boot}/entry.S ${boot}/mpu.c ${boot}/format.c
        ${core}/Startup/startup_stm32h747xx_${STM32_CORE}.s
        ${core}/Src/main.c ${core}/Src/stm32h7xx_hal_msp.c
        ${cube}/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c)
    if(STM32_CORE STREQUAL "CM7")
        target_sources(${target} PRIVATE ${boot}/memories.c ${boot}/clocks.c ${core}/Src/quadspi.c ${core}/Src/fmc.c)
        foreach(driver hal_qspi hal_sdram ll_fmc)
            target_sources(${target} PRIVATE ${hal_sources}/stm32h7xx_${driver}.c)
        endforeach()
    endif()
    foreach(driver hal hal_cortex hal_gpio hal_rcc hal_rcc_ex hal_pwr hal_pwr_ex
                   hal_flash hal_flash_ex hal_hsem hal_dma hal_dma_ex)
        target_sources(${target} PRIVATE ${hal_sources}/stm32h7xx_${driver}.c)
    endforeach()
    target_include_directories(${target} PRIVATE $<TARGET_PROPERTY:platform,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE STM32H747xx CORE_${STM32_CORE}
        USE_HAL_DRIVER USE_PWR_LDO_SUPPLY $<$<BOOL:${helper}>:GIGA_MEMORY_HELPER>)
    target_compile_options(${target} PRIVATE -Os -g3 -fno-builtin -Wall -Wextra -Wpedantic)
    target_link_options(${target} PRIVATE -nostartfiles --specs=nosys.specs
        -T${CMAKE_CURRENT_BINARY_DIR}/${target}.ld -Wl,--gc-sections -Wl,--print-memory-usage
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${target}.ld)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> ${CMAKE_CURRENT_BINARY_DIR}/${target}.bin
        VERBATIM)
endfunction()
giga_boot_image(bootstrap-${application_role} FALSE)
if(STM32_CORE STREQUAL "CM7")
    giga_boot_image(memory-helper TRUE)
endif()
