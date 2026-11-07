if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" OR NOT STM32_CORE MATCHES "^CM[47]$")
    message(FATAL_ERROR "Use an GIGA application preset")
endif()

set(HAL_BOARD arduino_giga)
set(RUNTIME_FILESYSTEM ON CACHE BOOL "Build FileX/LevelX filesystem support")
set(RUNTIME_STORAGE_REQUIRED OFF)
set(BUILD_TESTING OFF)
set(GIGA_GUI_MEMORY "")
set(GIGA_GUI_SECTIONS "")
set(GIGA_GUI_INTERNAL_SECTION "")
if(STM32_CORE STREQUAL "CM4")
    set(THREADX_ARCH cortex_m4)
    set(application_role io)
    set(GIGA_FLASH_ORIGIN 0x90100000)
    set(GIGA_FLASH_LENGTH 3M)
    set(GIGA_CODE_ORIGIN 0x90100000)
    set(GIGA_CODE_LENGTH 3M)
    if(GIGA_M4_SDRAM_CODE)
        set(GIGA_CODE_ORIGIN 0xC0400000)
        set(GIGA_CODE_LENGTH 2M)
    endif()
    set(GIGA_VECTOR_ORIGIN 0x10000000)
    set(GIGA_FAST_ORIGIN 0x10000800)
    # SRAM1 plus 66 KiB of SRAM2: worker preparation and IRQ paths remain in RAM.
    set(GIGA_FAST_LENGTH 192K)
    set(GIGA_TCM_ORIGIN 0x30030800)
    set(GIGA_TCM_LENGTH 0)
    set(GIGA_RAM_ORIGIN 0x30030800)
    set(GIGA_RAM_LENGTH 86K)
    set(GIGA_STACK_ORIGIN 0x30046000)
    set(GIGA_HEAP_REGION HEAP)
    # Exception-unwind records are data, not instruction-fetch hot paths.
    # Put them ahead of the M4 heap, retaining their signed PREL31 reach to SRAM.
    set(GIGA_FAST_UNWIND_REGION HEAP)
    include(${PROJECT_SOURCE_DIR}/cmake/giga_fast_code.cmake)
else()
    set(THREADX_ARCH cortex_m7)
    set(application_role ui)
    set(GIGA_FLASH_ORIGIN 0x90400000)
    # Code, fonts, images and meshes share one image up to the manifest at 0x90E00000.
    set(GIGA_FLASH_LENGTH 10M)
    set(GIGA_CODE_ORIGIN 0x90400000)
    set(GIGA_CODE_LENGTH 10M)
    set(GIGA_VECTOR_ORIGIN 0x20000000)
    set(GIGA_FAST_ORIGIN 0x00000000)
    # Keep CPU-only GUI bookkeeping out of AXI SRAM's nearly full general heap.
    # The lower ITCM half retains the renderer's existing <=24 KiB code budget.
    set(GIGA_FAST_LENGTH 32K)
    set(GIGA_TCM_ORIGIN 0x20000800)
    set(GIGA_TCM_LENGTH 110K)
    set(GIGA_RAM_ORIGIN 0x24000000)
    set(GIGA_RAM_LENGTH 256K)
    set(GIGA_STACK_ORIGIN 0x2001E000)
    set(GIGA_HEAP_REGION RAM)
    set(GIGA_FAST_UNWIND_REGION FAST)
    # Profiled mesh transform/raster paths exceed M7 I-cache working-set size.
    # Keep this initial renderer reservation below 24 KiB.
    set(GIGA_FAST_OBJECTS "")
    foreach(symbol "*_drawMesh*" "*uber_shader*" "*_discardBox*")
        string(APPEND GIGA_FAST_OBJECTS " *libproto_scene3d_renderer.a:Renderer.cpp.obj(.text.${symbol})")
    endforeach()
    set(giga_m7_fast_limit 24576)
    if(GIGA_MOTION_BENCH)
        # Campaign experiment: PC samples were distributed across runtime dispatch.
        # Measure instruction-RAM placement without changing normal firmware.
        set(giga_m7_fast_limit 65536)
        set(GIGA_FAST_LENGTH 64K)
        foreach(object backend.cpp tls.cpp Kernels.cpp CompactExpand.cpp FastControl.cpp FixedBench.cpp)
            string(APPEND GIGA_FAST_OBJECTS " */${object}.obj(.text* .rodata*) *:${object}.obj(.text* .rodata*)")
        endforeach()
        string(APPEND GIGA_FAST_OBJECTS " *libthreadx.a:*(.text* .rodata*)")
        foreach(symbol memcpy memmove memset malloc free _malloc_r _free_r __malloc_lock __malloc_unlock)
            string(APPEND GIGA_FAST_OBJECTS " *(.text.${symbol})")
        endforeach()
    endif()
    set(GIGA_GUI_MEMORY "GUI (rw) : ORIGIN = 0xC0600000, LENGTH = 2M")
    if(GIGA_MOTION_BENCH)
        # This explicit benchmark variant already reserves all ITCM for code.
        set(giga_gui_metadata_region RAM)
    else()
        string(APPEND GIGA_GUI_MEMORY "\n    GUI_INTERNAL (rw) : ORIGIN = 0x00008000, LENGTH = 32K")
        set(giga_gui_metadata_region GUI_INTERNAL)
    endif()
    set(GIGA_GUI_INTERNAL_SECTION ".gui_internal (NOLOAD) : ALIGN(32) {
      __gui_internal_start = .; KEEP(*(.gui_internal)) . = ALIGN(32);
      __gui_internal_end = .;
    } >${giga_gui_metadata_region}
    ASSERT(SIZEOF(.gui_internal) <= 24576, \"GUI bookkeeping exceeds 24 KiB\")")
    # Owner initializes every pixel/depth word before use. No bootstrap clear,
    # heap, general C++ objects or M4 SDRAM code overlap in this reservation.
    set(GIGA_GUI_SECTIONS ".gui_sdram (NOLOAD) : ALIGN(32) {
      __gui_sdram_start = .; *(.gui_sdram .gui_sdram.*) . = ALIGN(32);
      __gui_sdram_end = .;
    } >GUI
    .motion_sdram (NOLOAD) : ALIGN(32) {
      __motion_sdram_start = .; *(.motion_sdram .motion_sdram.*) . = ALIGN(32);
      __motion_sdram_end = .;
    } >GUI
    ASSERT(SIZEOF(.motion_sdram) <= 262144, \"Robot planning arena exceeds 256 KiB\")
    ASSERT(SIZEOF(.gui_sdram) + SIZEOF(.motion_sdram) <= 2097152, \"UI SDRAM reservations overlap region end\")
    ASSERT(SIZEOF(.fast) + SIZEOF(.ARM.fast_extab) + SIZEOF(.ARM.fast_exidx) <= ${giga_m7_fast_limit}, \"M7 ITCM reservation exceeded\")
    ASSERT(SIZEOF(.gui_sdram) <= 1601536, \"3D buffers exceed their budget\")")
    # CPU-only metadata, console rings and runtime stacks use private DTCM.
    # USB FS uses FIFO copies (no DMA); its Cube buffers remain in AXI SRAM.
    # Keep the general heap in AXI, so arbitrary application buffers remain
    # accessible to peripherals. M4 owns its separate AXI half unchanged.
    set(GIGA_TCM_DATA "*libruntime_libc.a:filesystem.cpp.obj(.data .data.*)")
    set(GIGA_TCM_BSS "*startup.cpp.obj(.bss .bss.*)
      *libhal.a:console.cpp.obj(.bss .bss.*)
      *libruntime_libc.a:filesystem.cpp.obj(.bss .bss.*)
      *(.bss.motion_bench_stack)
      *(.bss.*reaper_stack*)")

endif()

if(GIGA_MOTION_BENCH AND STM32_CORE STREQUAL "CM7")
    # The larger campaign command stack is static AXI storage, not the UI heap.
    string(REPLACE "*startup.cpp.obj(.bss .bss.*)" "" GIGA_TCM_BSS "${GIGA_TCM_BSS}")
endif()

add_library(project_compiler_settings INTERFACE)
target_compile_features(project_compiler_settings INTERFACE cxx_std_26)
set(THREADX_TOOLCHAIN gnu)
set(TX_USER_FILE ${PROJECT_SOURCE_DIR}/platform/runtime/tx_user.h)
add_subdirectory(external/threadx)
target_include_directories(threadx PUBLIC ${PROJECT_SOURCE_DIR}/platform)
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
        ${core}/Src/crc.c ${core}/Src/dma2d.c ${core}/Src/ltdc.c ${core}/Src/i2c.c
        ${core}/Src/usb_device.c ${core}/Src/usbd_conf.c ${core}/Src/usbd_desc.c ${core}/Src/usbd_cdc_if.c)
    foreach(driver hal_crc hal_crc_ex hal_pcd hal_pcd_ex ll_usb hal_ltdc hal_ltdc_ex hal_dma2d hal_i2c hal_i2c_ex)
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
if(STM32_CORE STREQUAL "CM4" AND NOT GIGA_MOTION_BENCH AND NOT GIGA_STEP_BENCH)
    # Keep the local executor API in the normal image before ICC exposes it.
    # Dead stripping must not hide its RAM/code budget from the production audit.
    target_link_options(${PROJECT_NAME} PRIVATE
        LINKER:--undefined=_ZN2io6motion10Controller7executeERKNS0_8Executor7CommandE
        LINKER:--undefined=_ZN2io6motion10Controller14renewExecutionEy
        LINKER:--undefined=_ZN2io6motion10Controller13stopExecutionEy
        LINKER:--undefined=_ZNK2io6motion10Controller15executionStatusEy)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${application_role}-controller)
target_link_libraries(${PROJECT_NAME} PRIVATE platform project_compiler_settings)
target_compile_options(${PROJECT_NAME} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra;-Wpedantic>")
if(GIGA_MOTION_BENCH)
    # The campaign command stack also collects bounded memory/stack snapshots.
    target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=16384U)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE RUNTIME_APPLICATION_THREAD_STACK_SIZE=8192U)
endif()
if(STM32_CORE STREQUAL "CM4" AND GIGA_M4_SDRAM_CODE)
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
set(GIGA_PROFILE_COLD "")
set(GIGA_PROFILE_MEMORY "")
set(GIGA_PROFILE_SECTIONS "")
if(PROTOMATE_PROFILING)
    if(STM32_CORE STREQUAL "CM4")
        set(profile_base 0xC0400000)
        set(profile_summary 0xC0420000)
    else()
        set(profile_base 0xC0440000)
        set(profile_summary 0xC0460000)
    endif()
    set(GIGA_PROFILE_MEMORY "PROFILE_TRACE (rw) : ORIGIN = ${profile_base}, LENGTH = 128K
    PROFILE_SUMMARY (rw) : ORIGIN = ${profile_summary}, LENGTH = 32K")
    set(GIGA_PROFILE_COLD ".profile_setup : ALIGN(4) {
      *libplatform_profiling.a:Recorder.c.obj(.text.profile_initialize .text.profile_arm .text.profile_allow_arm .text.profile_service .text.profile_stack .text.profile_status .text.profile_measure .text.profile_queue .text.profile_subscription .text.profile_heap .text.profile_allocation .text.profile_generation)
      *libthreadx.a:tx_trace_initialize.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_enable.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_object_register.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_object_unregister.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_event_filter.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_event_unfilter.c.obj(.text* .rodata*)
      *libthreadx.a:tx_trace_buffer_full_notify.c.obj(.text* .rodata*)
    } >CODE AT>FLASH")
    set(GIGA_PROFILE_SECTIONS ".profile_trace (NOLOAD) : ALIGN(32) { KEEP(*(.profile_trace)) } >PROFILE_TRACE
    .profile_summary (NOLOAD) : ALIGN(32) { KEEP(*(.profile_summary)) } >PROFILE_SUMMARY
    ASSERT(SIZEOF(.profile_trace) <= 131072, \"Trace buffer overflow\")
    ASSERT(SIZEOF(.profile_summary) <= 32768, \"Trace summaries overflow\")")
endif()
set(GIGA_BENCH_SECTIONS "")
set(GIGA_BENCH_MEMORY "")
if(GIGA_MOTION_API_TEST)
    add_compile_definitions(GIGA_MOTION_API_TEST=1)
endif()
if(GIGA_MOTION_BENCH)
    add_compile_definitions(GIGA_MOTION_BENCH=1)
    if(STM32_CORE STREQUAL "CM4")
        set(bench_workspace 0xC0400000)
        set(bench_trace 0xC0420000)
        set(bench_buffers 0xC0430000)
        set(bench_runs 0xC0480000)
    else()
        set(bench_workspace 0xC0440000)
        set(bench_trace 0xC0460000)
        set(bench_buffers 0xC0470000)
        set(bench_runs 0xC04C0000)
    endif()
    set(GIGA_BENCH_MEMORY "BENCH_WORK (rw) : ORIGIN = ${bench_workspace}, LENGTH = 256K
    BENCH_RUNS (rw) : ORIGIN = ${bench_runs}, LENGTH = 256K
    BENCH_FAST (rw) : ORIGIN = 0xC0500000, LENGTH = 64K")
    set(GIGA_BENCH_SECTIONS ".motion_bench_shared 0x3800D000 (NOLOAD) : { KEEP(*(.motion_bench_shared)) } >SHARED
    .motion_bench_workspace (NOLOAD) : ALIGN(32) { KEEP(*(.motion_bench_workspace)) } >BENCH_WORK
    .motion_bench_trace ${bench_trace} (NOLOAD) : ALIGN(32) { KEEP(*(.motion_bench_trace)) } >BENCH_WORK
    .motion_bench_buffers ${bench_buffers} (NOLOAD) : ALIGN(32) { KEEP(*(.motion_bench_buffers)) } >BENCH_WORK
    .motion_bench_runs (NOLOAD) : ALIGN(32) { KEEP(*(.motion_bench_runs)) } >BENCH_RUNS
    .motion_bench_fast (NOLOAD) : ALIGN(32) { KEEP(*(.motion_bench_fast)) } >BENCH_FAST
    ASSERT(SIZEOF(.motion_bench_fast) <= 65536, \"Benchmark fast channel overflow\")
    ASSERT(SIZEOF(.motion_bench_runs) <= 262144, \"Benchmark runs overflow\")
    ASSERT(SIZEOF(.motion_bench_buffers) <= 65536, \"Benchmark buffers overflow\")
    ASSERT(SIZEOF(.motion_bench_trace) <= 65536, \"Benchmark trace overflow\")
    ASSERT(ADDR(.intercore) + SIZEOF(.intercore) <= 0x3800D000, \"Benchmark overlaps ICC\")
    ASSERT(SIZEOF(.motion_bench_shared) <= 8192, \"Benchmark overlaps handoff\")
    ASSERT(SIZEOF(.motion_bench_workspace) <= 131072, \"Benchmark workspace overflow\")")
endif()
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
add_subdirectory(platform/profiling)
add_subdirectory(platform/hal)
add_subdirectory(platform/runtime)
add_subdirectory(src)
include(${PROJECT_SOURCE_DIR}/cmake/bootstrap_giga.cmake)

if(STM32_CORE STREQUAL "CM4")
    set(fast_directories Application platform runtime_libc)
    foreach(owner proto_io proto_common hal hal_drivers hal_devices hal_devices_util runtime_synchronization runtime_libstdcxx runtime_threadx)
        if(TARGET ${owner})
            list(APPEND fast_directories ${owner})
        endif()
    endforeach()
    foreach(source IN LISTS giga_fast_sources)
        set(fast_opt -Os)
        if(source MATCHES "(StepGenerator|StepHardware|FixedBench|CompactExpand|ProfileBench|ProfileSchedule|Executor)\\.cpp$")
            set(fast_opt -O2)
        endif()
        set_source_files_properties(${PROJECT_SOURCE_DIR}/${source}
            TARGET_DIRECTORY ${fast_directories}
            PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic;${fast_opt};-include;${PROJECT_SOURCE_DIR}/platform/boot/giga/fast_unwind.h")
    endforeach()
    target_compile_options(threadx PRIVATE -Os)
endif()

if(STM32_CORE STREQUAL "CM7" AND GIGA_MOTION_BENCH)
    target_compile_options(threadx PRIVATE -Os)
    foreach(source platform/runtime/libstdcxx/backend.cpp platform/runtime/libstdcxx/tls.cpp examples/giga_motion_bench/Kernels.cpp examples/giga_motion_bench/FastControl.cpp examples/giga_motion_bench/FixedBench.cpp)
        set_property(SOURCE ${PROJECT_SOURCE_DIR}/${source}
            TARGET_DIRECTORY Application runtime_libstdcxx APPEND PROPERTY COMPILE_OPTIONS
            -include ${PROJECT_SOURCE_DIR}/platform/boot/giga/fast_unwind.h)
        if(source MATCHES "platform/runtime|FastControl")
            set_property(SOURCE ${PROJECT_SOURCE_DIR}/${source} TARGET_DIRECTORY Application runtime_libstdcxx APPEND PROPERTY COMPILE_OPTIONS -Os)
        endif()
    endforeach()
endif()

target_include_directories(runtime_threadx PRIVATE $<TARGET_PROPERTY:platform,INCLUDE_DIRECTORIES>)
target_compile_definitions(runtime_threadx PRIVATE $<TARGET_PROPERTY:platform,COMPILE_DEFINITIONS>)
