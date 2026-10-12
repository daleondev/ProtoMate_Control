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

    foreach(assignment "PF6.GPIO_Label=ESC_CS_N" "PF6.PinState=GPIO_PIN_SET"
                       "PF6.Signal=GPIO_Output" "PF7.Signal=SPI5_SCK"
                       "PF8.Signal=SPI5_MISO" "PF9.Signal=SPI5_MOSI"
                       "SPI5.Mode=SPI_MODE_MASTER" "SPI5.DataSize=SPI_DATASIZE_8BIT"
                       "SPI5.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_128"
                       "SPI5.CLKPolarity=SPI_POLARITY_LOW" "SPI5.CLKPhase=SPI_PHASE_1EDGE"
                       "SPI5.MasterKeepIOState=SPI_MASTER_KEEP_IO_STATE_ENABLE")
        cubemx_require_text("${cubemx_directory}/CubeMX.ioc" "${assignment}\n"
            "LAN9255 bench requires SPI5 mode 0 and a deasserted PF6 chip select.")
    endforeach()
    foreach(required "hspi5.Init.DataSize = SPI_DATASIZE_8BIT;"
                     "hspi5.Init.CLKPolarity = SPI_POLARITY_LOW;"
                     "hspi5.Init.CLKPhase = SPI_PHASE_1EDGE;"
                     "hspi5.Init.NSS = SPI_NSS_SOFT;"
                     "hspi5.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_128;"
                     "hspi5.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;"
                     "GPIO_AF5_SPI5")
        cubemx_require_text("${cubemx_directory}/Src/spi.c" "${required}"
            "Regenerate the LAN9255 SPI5 configuration.")
    endforeach()
    cubemx_require_text("${cubemx_directory}/Src/gpio.c"
        "HAL_GPIO_WritePin(ESC_CS_N_GPIO_Port, ESC_CS_N_Pin, GPIO_PIN_SET);"
        "ESC chip select must start deasserted.")

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
    foreach(fault NMI HardFault MemManage BusFault UsageFault)
        cubemx_require_text("${cubemx_directory}/Src/stm32h7xx_it.c"
            "hal_fault_handler(\"${fault}\");"
            "Cortex fatal exceptions must enter the motor shutdown/panic path.")
    endforeach()

    cubemx_require_text("${cubemx_directory}/CubeMX.ioc"
        "NVIC.TIM6_DAC_IRQn=true\\:14\\:0"
        "TIM6 must preempt ThreadX's lowest-priority PendSV idle handler.")
    cubemx_require_text("${cubemx_directory}/Inc/stm32h7xx_hal_conf.h"
        "TICK_INT_PRIORITY            (14UL)"
        "The HAL tick priority must match CubeMX and preempt ThreadX idle.")

    set(linker_script "${cubemx_directory}/STM32H753XX_FLASH.ld")
    foreach(assignment "PF2.GPIO_Label=M1_ALM" "PF2.Signal=GPXTI2"
                       "PF2.GPIO_PuPd=GPIO_PULLUP"
                       "PF2.GPIO_ModeDefaultEXTI=GPIO_MODE_IT_RISING")
        cubemx_require_text("${cubemx_directory}/CubeMX.ioc" "${assignment}\n"
            "DM542T ALM must be a pulled-up, rising-edge fault input on PF2.")
    endforeach()
    foreach(required "GPIO_InitStruct.Pin = M1_ALM_Pin;"
                     "HAL_GPIO_Init(M1_ALM_GPIO_Port, &GPIO_InitStruct);"
                     "__HAL_RCC_GPIOF_CLK_ENABLE();"
                     "HAL_NVIC_SetPriority(EXTI2_IRQn, 5, 0);"
                     "HAL_NVIC_EnableIRQ(EXTI2_IRQn);")
        cubemx_require_text("${cubemx_directory}/Src/gpio.c" "${required}"
            "Regenerate the DM542T ALM GPIO and EXTI2 initialization.")
    endforeach()
    cubemx_require_text("${cubemx_directory}/Src/stm32h7xx_it.c"
        "HAL_GPIO_EXTI_IRQHandler(M1_ALM_Pin);" "DM542T ALM interrupt dispatch is missing.")
    foreach(assignment "PD5.Signal=USART2_TX" "PD6.Signal=USART2_RX"
                       "PD0.GPIO_Label=M2_INDEX" "PD1.GPIO_Label=M3_INDEX"
                       "PD0.Signal=GPXTI0" "PD1.Signal=GPXTI1"
                       "PD0.GPIO_PuPd=GPIO_PULLDOWN" "PD1.GPIO_PuPd=GPIO_PULLDOWN"
                       "PD0.GPIO_ModeDefaultEXTI=GPIO_MODE_IT_RISING_FALLING"
                       "PD1.GPIO_ModeDefaultEXTI=GPIO_MODE_IT_RISING_FALLING"
                       "PD4.GPIO_Label=M2_DIAG" "PD3.GPIO_Label=M3_DIAG"
                       "PD4.Signal=GPXTI4" "PD3.Signal=GPXTI3"
                       "PD3.GPIO_PuPd=GPIO_PULLDOWN" "PD4.GPIO_PuPd=GPIO_PULLDOWN"
                       "PD3.GPIO_ModeDefaultEXTI=GPIO_MODE_IT_RISING"
                       "PD4.GPIO_ModeDefaultEXTI=GPIO_MODE_IT_RISING"
                       "USART2.BaudRate=115200" "USART2.FIFOMode=FIFOMODE_ENABLE")
        cubemx_require_text("${cubemx_directory}/CubeMX.ioc" "${assignment}\n"
            "The shared TMC2209 UART and DIAG assignment was overwritten.")
    endforeach()
    foreach(required "huart2.Instance = USART2;" "huart2.Init.BaudRate = 115200;"
                     "huart2.Init.WordLength = UART_WORDLENGTH_8B;"
                     "huart2.Init.StopBits = UART_STOPBITS_1;"
                     "huart2.Init.Parity = UART_PARITY_NONE;"
                     "HAL_UARTEx_EnableFifoMode(&huart2)" "GPIO_AF7_USART2")
        cubemx_require_text("${cubemx_directory}/Src/usart.c" "${required}"
            "Regenerate USART2: 115200 8N1 with RX FIFO for request echo and reply.")
    endforeach()
    # The onboard STLINK-V3 MCO must be configured separately to HSE/5.
    # Its 25 MHz crystal supplies the target's 5 MHz HSE bypass input.
    foreach(assignment "RCC.HSE_VALUE=5000000" "RCC.DIVM1=1" "RCC.DIVN1=192"
                       "RCC.DIVQ1=24" "RCC.SYSCLKFreq_VALUE=480000000"
                       "RCC.Tim2OutputFreq_Value=240000000")
        cubemx_require_text("${cubemx_directory}/CubeMX.ioc" "${assignment}\n"
            "The clock tree must match the ST-Link crystal-derived 5 MHz MCO.")
    endforeach()
    foreach(required "RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;"
                     "RCC_OscInitStruct.PLL.PLLM = 1;"
                     "RCC_OscInitStruct.PLL.PLLN = 192;"
                     "RCC_OscInitStruct.PLL.PLLP = 2;"
                     "RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;")
        cubemx_require_text("${cubemx_directory}/Src/main.c" "${required}"
            "Regenerate the 5 MHz HSE bypass clock initialization.")
    endforeach()
    cubemx_require_text("${cubemx_directory}/Inc/stm32h7xx_hal_conf.h"
        "#define HSE_VALUE    (5000000UL)"
        "HAL clock calculations must use the 5 MHz ST-Link MCO input.")
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
        "TIM7.Prescaler=239"
        "TIM7.Period=999"
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
                     "htim2.Init.Period = 4294967294;" "TIM_OCMODE_TOGGLE"
                     "htim7.Init.Prescaler = 239;" "htim7.Init.Period = 999;"
                     "HAL_NVIC_SetPriority(TIM7_IRQn, 5, 0);")
        cubemx_require_text("${cubemx_directory}/Src/tim.c" "${required}"
            "Regenerate the TIM2 step engine, TIM7 completion monitor and TIM5 runtime initialization.")
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
