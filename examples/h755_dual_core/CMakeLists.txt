if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" OR NOT H755_CORE MATCHES "^CM[47]$")
    message(FATAL_ERROR "Use an H755 preset selecting H755_CORE=CM4 or CM7")
endif()

if(H755_CORE STREQUAL "CM4")
    set(controller io)
else()
    set(controller ui)
endif()

set(cube ${PROJECT_SOURCE_DIR}/external/CubeMX)
set(core ${cube}/${H755_CORE})
set(hal ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver)
set(linker_script ${core}/stm32h755xx_flash_${H755_CORE}.ld)
set(image ${controller}-controller)

# Generated startup and peripheral configuration, linked against our existing
# driver submodules rather than the workstation-specific Cube repository paths.
add_library(h755_platform OBJECT
    ${core}/Startup/startup_stm32h755xx_${H755_CORE}.s
    ${core}/Src/main.c
    ${core}/Src/gpio.c
    ${core}/Src/stm32h7xx_hal_msp.c
    ${core}/Src/stm32h7xx_it.c
    ${core}/Src/sysmem.c
    ${core}/Src/syscalls.c
    ${cube}/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c
    ${hal}/Src/stm32h7xx_hal.c
    ${hal}/Src/stm32h7xx_hal_cortex.c
    ${hal}/Src/stm32h7xx_hal_gpio.c
    ${hal}/Src/stm32h7xx_hal_rcc.c
    ${hal}/Src/stm32h7xx_hal_rcc_ex.c
    ${hal}/Src/stm32h7xx_hal_pwr.c
    ${hal}/Src/stm32h7xx_hal_pwr_ex.c
    ${hal}/Src/stm32h7xx_hal_flash.c
    ${hal}/Src/stm32h7xx_hal_flash_ex.c
    ${hal}/Src/stm32h7xx_hal_hsem.c
)
target_include_directories(h755_platform PUBLIC
    ${core}/Inc
    ${hal}/Inc
    ${hal}/Inc/Legacy
    ${PROJECT_SOURCE_DIR}/external/cmsis-device-h7/Include
    ${PROJECT_SOURCE_DIR}/external/cmsis-core/CMSIS/Core/Include
)
target_compile_definitions(h755_platform PUBLIC
    CORE_${H755_CORE} STM32H755xx USE_HAL_DRIVER USE_PWR_DIRECT_SMPS_SUPPLY
    # Explicit per-core vector tables: bank 1 for M7, bank 2 for M4.
    USER_VECT_TAB_ADDRESS
    $<$<CONFIG:Debug>:DEBUG>
)
target_compile_features(h755_platform PRIVATE c_std_11)

add_executable(${image} ${controller}/main.cpp)
target_link_libraries(${image} PRIVATE h755_platform)
target_compile_features(${image} PRIVATE cxx_std_26)
target_compile_options(${image} PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(${image} PRIVATE
    -T${linker_script}
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_BINARY_DIR}/${image}.map
)
set_target_properties(${image} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LINK_DEPENDS ${linker_script}
)
add_custom_command(TARGET ${image} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${image}> ${CMAKE_BINARY_DIR}/${image}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${image}> ${CMAKE_BINARY_DIR}/${image}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${image}>
    VERBATIM
)
