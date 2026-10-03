function(cubemx_require_text file_path required_text failure_reason)
    file(READ "${file_path}" file_contents)
    string(FIND "${file_contents}" "${required_text}" match_position)
    if(match_position EQUAL -1)
        message(FATAL_ERROR
            "CubeMX generation guard failed: ${failure_reason}\n"
            "Missing '${required_text}' in ${file_path}"
        )
    endif()
endfunction()

function(verify_cubemx_generation)
    set(cubemx_directory "${PROJECT_SOURCE_DIR}/external/CubeMX")

    cubemx_require_text(
        "${cubemx_directory}/CubeMX.ioc"
        "ProjectManager.KeepUserCode=true"
        "CubeMX must preserve project hooks during regeneration."
    )
    cubemx_require_text(
        "${cubemx_directory}/CubeMX.ioc"
        "ProjectManager.NoMain=true"
        "CubeMX must not generate an application main function."
    )
    cubemx_require_text(
        "${cubemx_directory}/Src/main.c"
        "#include \"hal/panic.h\""
        "The generated error handler lost its project panic declaration."
    )
    cubemx_require_text(
        "${cubemx_directory}/Src/main.c"
        "hal_error_handler();"
        "The generated error handler no longer delegates to the HAL panic path."
    )

    set(linker_script "${cubemx_directory}/STM32H753XX_FLASH.ld")
    foreach(assignment
        "PA0.Signal=S_TIM2_CH1_ETR"
        "PB10.Signal=S_TIM2_CH3"
        "PB11.Signal=S_TIM2_CH4"
        "PE12.GPIO_Label=M1_DIR"
        "PE13.GPIO_Label=M2_DIR"
        "PE14.GPIO_Label=M3_DIR"
        "PE15.GPIO_Label=STEPPERS_EN_N"
        "TIM2.Prescaler=23"
        "TIM2.Period=4294967294"
        "TIM5.Prescaler=239"
        "TIM5.Period=4294967295"
        "Dma.TIM2_CH1.0.Instance=DMA1_Stream0"
        "Dma.TIM2_CH3.1.Instance=DMA1_Stream1"
        "Dma.TIM2_CH4.2.Instance=DMA1_Stream2"
        "Dma.TIM2_CH2.3.Instance=DMA1_Stream3"
        "CORTEX_M7.BaseAddress_S-Cortex_Memory_Protection_Unit_Region3_Settings_S=0x30000000"
        "CORTEX_M7.Size_S-Cortex_Memory_Protection_Unit_Region3_Settings_S=MPU_REGION_SIZE_16KB"
        "CORTEX_M7.IsCacheable_S-Cortex_Memory_Protection_Unit_Region3_Settings_S=MPU_ACCESS_NOT_CACHEABLE")
        cubemx_require_text("${cubemx_directory}/CubeMX.ioc" "${assignment}"
            "The shared step engine or runtime resource assignment was overwritten.")
    endforeach()
    foreach(required "htim5.Instance = TIM5;" "htim2.Init.Prescaler = 23;"
                     "htim2.Init.Period = 4294967294;" "TIM_OCMODE_TOGGLE")
        cubemx_require_text("${cubemx_directory}/Src/tim.c" "${required}"
            "Regenerate the TIM2 DMA step engine and TIM5 runtime initialization.")
    endforeach()
    cubemx_require_text("${linker_script}" ".StepDmaSection 0x30000000"
        "The dedicated non-cacheable step DMA allocation was overwritten.")
    cubemx_require_text("${cubemx_directory}/Src/main.c" "MPU_InitStruct.BaseAddress = 0x30000000;"
        "The non-cacheable step DMA MPU region was overwritten.")
    cubemx_require_text(
        "${linker_script}"
        "DTCM_STACK"
        "The project interrupt-stack layout was overwritten."
    )
    cubemx_require_text(
        "${cubemx_directory}/CubeMX.ioc"
        "PC8.Signal=SDMMC1_D0"
        "The SDMMC pin configuration was overwritten."
    )
    cubemx_require_text(
        "${cubemx_directory}/CubeMX.ioc"
        "PG6.Signal=QUADSPI_BK1_NCS"
        "The W25Q128 QuadSPI pin configuration was overwritten."
    )
    cubemx_require_text(
        "${linker_script}"
        "AXI_SRAM"
        "The project AXI SRAM and heap layout was overwritten."
    )
    cubemx_require_text(
        "${linker_script}"
        ".EthBufferSection"
        "The Ethernet DMA buffer layout was overwritten."
    )
    cubemx_require_text(
        "${linker_script}"
        ".tdata"
        "The C++ thread-local storage layout was overwritten."
    )
endfunction()
