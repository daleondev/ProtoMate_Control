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
