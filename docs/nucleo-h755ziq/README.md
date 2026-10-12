# H755 application setup

The two images run the actual `io::Application` and `ui::Application` classes
with separate ThreadX kernels and the shared project C++ runtime implementation.
P2P, CLI, motor hardware and external storage are disabled for this stage.

## Ownership

| Resource | M4 / IO | M7 / UI |
| --- | --- | --- |
| Entry point | `src/io/main.cpp` | `src/ui/main.cpp` |
| ThreadX port | `cortex_m4/gnu` | `cortex_m7/gnu` |
| Program flash | `0x08100000`, 1 MiB | `0x08000000`, 1 MiB |
| Globals, TLS and heap | D2 SRAM `0x30000000`, 280 KiB | AXI SRAM `0x24000000`, 512 KiB |
| Main/interrupt stack | `0x30046000`, 8 KiB | DTCM `0x2001E000`, 8 KiB |
| Scheduler and HAL tick | Core-local SysTick, 1 kHz | Core-local SysTick, 1 kHz |
| Runtime counter | TIM5, 32-bit, 1 MHz | TIM12, 16-bit, 1 MHz |
| Status LED | LD1 green / PB0, 250 ms per level | LD2 yellow / PE1, 500 ms per level |

Each application owns its mutexes, condition variables, threads and runtime state.
`hal::board::create_runtime_timer()` selects the counter; the runtime does not
select a board peripheral itself. TIM2 is free for the later IO motion port.

M7 owns shared clock and supply initialization (64 MHz HSI, direct SMPS). HSEM 0
releases M4 after this setup. This startup handshake is the only inter-core
coordination. Both images must be present and both cores enabled at boot.
M7 caches are enabled over its private memory. SRAM4 at `0x38000000` is unused;
shared-memory reservation, cache policy and transport notification remain later work.

The HAL board factory only exposes the LED owned by the selected core. Its
current driver build includes GPIO and general timers. The shared RTC and RNG
are unassigned: their factories return unavailable, and `std::random_device`
does not produce substitute random data. Without an RTC, `system_clock` uses
the runtime's elapsed-time fallback rather than a valid wall-clock date. Both
`steady_clock` and that fallback currently have 1 ms resolution. The counters
are available through `ITimer`; high-resolution wall-clock interpolation remains
inactive until an RTC is provided.

`RUNTIME_FILESYSTEM=OFF` removes FileX/LevelX and storage startup from these
images. The C/C++ threading, allocation, TLS, exceptions and clocks use the same
runtime as the other builds. No UART console is initialized yet. Panic details
are retained in each core's `hal_last_panic` for SWD inspection.

## Build

From the repository root:

```sh
cmake --preset h755-io-debug
cmake --build --preset h755-io-debug
cmake --preset h755-ui-debug
cmake --build --preset h755-ui-debug
```

Use `h755-io-release` and `h755-ui-release` for optimized images. The initial
bare-metal samples remain available as `h755-led-io-debug` and
`h755-led-ui-debug`, also with Release variants.

CubeMX owns GPIO/timer setup and startup assembly under `external/CubeMX`.
The runtime owns its interrupt integration, TLS/heap linker sections and
allocator under `platform/runtime/stm32`, so regeneration cannot overwrite them.
`cmake/application_h755.cmake` selects the core port, sources and private memory.
It renames generated SysTick/PendSV/fault handlers so the runtime handlers own
those vectors. The generated timer configuration remains in CubeMX.

## Flash and test

Connect the board's ST-LINK USB port. No peripherals are needed. Flash both
images before releasing reset:

```sh
openocd -f cmake/openocd/nucleo-h755ziq.cfg \
  -c "init; reset halt; targets stm32h7x.cpu0" \
  -c "flash write_image erase build/h755-ui-debug/ui-controller.elf" \
  -c "verify_image build/h755-ui-debug/ui-controller.elf" \
  -c "flash write_image erase build/h755-io-debug/io-controller.elf" \
  -c "verify_image build/h755-io-debug/io-controller.elf" \
  -c "reset run; shutdown"
```

Green should blink at approximately 2 Hz and yellow at approximately 1 Hz.
They are controlled by the annotated application workers through `ILed` and
condition-variable waits. A reset or USB reconnect should restart both workers.

For debugger inspection, each image has four ThreadX threads: the kernel timer
thread, runtime main thread, cleanup/reaper thread and its application worker.
Both `_tx_timer_system_clock` counters should advance; `hal_last_panic`, CFSR and
HFSR should remain clear. ThreadX normally idles inside PendSV when no thread is
ready, so stopping there is expected. Use the ELF belonging to the selected core.

Debug and Release builds pass without compiler warnings. Both configurations
passed flash readback verification, three reset/startup checks, live application
thread checks and LED GPIO-rate checks, with clear fault/panic state. Linux
application lifecycle tests also pass, including starting and stopping without
a transport. The Debug pair is installed for further development. No lint or
sanitizer runs were performed for this intermediate step.
