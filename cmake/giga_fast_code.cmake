# Every source listed here is compiled with symbols and optimization even in
# Debug, and its code/constants/unwind metadata are copied to instruction RAM.
set(giga_fast_sources
    platform/hal/drivers/impl/StepGenerator.cpp
    platform/hal/drivers/impl/StepOutput.cpp
    platform/hal/drivers/impl/stm32/StepHardware.cpp
    platform/hal/drivers/impl/stm32/QuadratureEncoder.cpp
    platform/hal/drivers/impl/stm32/Gpio.cpp
    platform/hal/drivers/impl/stm32/Timer.cpp
    platform/hal/drivers/impl/stm32/InterCoreChannel.cpp
    platform/hal/stm32/FaultShutdown.cpp
    platform/hal/devices/impl/IndexFeedback.cpp
    platform/hal/devices/impl/QuadratureEncoderFeedback.cpp
    platform/hal/devices/impl/Dm542tDriver.cpp
    platform/hal/devices/impl/Tmc2209Driver.cpp
    platform/hal/devices/util/DriverFaultMonitor.cpp
    src/io/motion/StepperMotor.cpp
    src/io/motion/Controller.cpp
    src/io/motion/Sequence.cpp
    src/common/motion/Profile.cpp
    platform/runtime/synchronization/Notification.cpp
    platform/runtime/libstdcxx/clock.cpp
    platform/runtime/libstdcxx/SteadyClock.cpp
    platform/runtime/libstdcxx/atomic.cpp
    platform/runtime/libstdcxx/backend.cpp
    platform/runtime/threadx/Support.cpp
    platform/runtime/stm32/initialize.cpp
    platform/runtime/libc/libc.c
    platform/runtime/libc/cpu_clock.c
    platform/runtime/startup.cpp
    platform/hal/stm32/hal.cpp)
if(GIGA_STEP_BENCH)
    list(APPEND giga_fast_sources examples/giga_step_bench/IoBench.cpp)
endif()
set(GIGA_FAST_OBJECTS "")
foreach(source IN LISTS giga_fast_sources)
    get_filename_component(name ${source} NAME)
    string(APPEND GIGA_FAST_OBJECTS " *${name}.obj(.text* .rodata*)")
endforeach()
# RTOS wakeups and software floating-point helpers are used by ISR callbacks.
string(APPEND GIGA_FAST_OBJECTS " *libthreadx.a:*(.text* .rodata*) *libgcc.a:*(.text* .rodata*)")

string(APPEND GIGA_FAST_OBJECTS " *stm32h7xx_hal_gpio.c.obj(.text* .rodata*)")
set(giga_fast_functions
    memcpy memcmp memset ceil sqrt cbrt __ieee754_sqrt __errno
    malloc free _malloc_r _free_r __malloc_lock __malloc_unlock
    HAL_HSEM_FastTake HAL_HSEM_Release HAL_HSEM_IRQHandler HAL_HSEM_ActivateNotification HAL_IncTick
    _ZNSt3_V216generic_categoryEv
    _ZNSt6chronossIxSt5ratioILx1ELx1000000000EExS2_EEDaRKNS_8durationIT_T0_EERKNS3_IT1_T2_EE)
foreach(symbol IN LISTS giga_fast_functions)
    string(APPEND GIGA_FAST_OBJECTS " *(.text.${symbol})")
endforeach()
