# NUCLEO-H755ZI-Q: first dual-core test

Two bare-metal applications verify the cores, clock initialization and GPIO
ownership. They have independent entry points, vector tables, SysTick timers and
memory regions. ThreadX, the C++ thread runtime, p2p and robot functionality are
not linked into these images.

| Application | Core | LED | Blink period | Flash | RAM |
| --- | --- | --- | --- | --- | --- |
| [IO](io/main.cpp) | Cortex-M4 | LD1 green, PB0 | 250 ms on / 250 ms off | `0x08100000`, 1 MiB | D2 SRAM, M4 alias `0x10000000`, 288 KiB |
| [UI](ui/main.cpp) | Cortex-M7 | LD2 yellow, PE1 | 500 ms on / 500 ms off | `0x08000000`, 1 MiB | DTCM `0x20000000`, 128 KiB |

Both cores use the internal 64 MHz HSI clock. M7 configures the shared clocks and
the board's default direct SMPS supply. M4 waits in STOP until M7 releases HSEM 0,
then initializes its HAL and GPIO. This is ST's startup synchronization sequence;
there is no application data exchange. Both images must be present for startup.

## Build

From the repository root, with CMake, Ninja and Arm GCC installed:

```sh
cmake --preset h755-led-io-debug
cmake --build --preset h755-led-io-debug
cmake --preset h755-led-ui-debug
cmake --build --preset h755-led-ui-debug
```

The corresponding optimized presets are `h755-led-io-release` and
`h755-led-ui-release`. Each build produces its own `.elf`, `.hex`, `.bin` and `.map`
in `build/<preset>/`. These presets run without the project HAL/runtime integration.

## Flash and observe

Connect the H755 board's ST-LINK USB connector. Use the default board supply and
LED solder-bridge configuration. No external peripherals or UART are needed.

Program both images in one OpenOCD session, keeping both cores halted until
programming and verification finish:

```sh
openocd -f cmake/openocd/nucleo-h755ziq.cfg \
  -c "init; reset halt; targets stm32h7x.cpu0" \
  -c "flash write_image erase build/h755-led-ui-debug/ui-controller.elf" \
  -c "verify_image build/h755-led-ui-debug/ui-controller.elf" \
  -c "flash write_image erase build/h755-led-io-debug/io-controller.elf" \
  -c "verify_image build/h755-led-io-debug/io-controller.elf" \
  -c "reset run; shutdown"
```

The M7 debug target can program both flash banks. The OpenOCD configuration uses
native ST-Link DAP and enables both core targets; the old HLA transport does not
provide dual-core debugging. This command changes program flash, not boot option
bytes. It assumes both cores are enabled at boot, with M7 booting from
`0x08000000` and M4 from `0x08100000`.

Green should blink twice per second and yellow once per second. Press RESET,
then unplug and reconnect USB: both should resume with the same behavior without
a debugger attached. If either does not blink, inspect both cores' PCs and boot
settings first; `Error_Handler()` traps initialization/startup failures. The
startup timeout intentionally stops M7 if the M4 image is missing or not running.

## CubeMX and source ownership

Open [CubeMX.ioc](../../external/CubeMX/CubeMX.ioc). It targets
`STM32H755ZITx` / `NUCLEO-H755ZI-Q`, with PB0 assigned to CortexM4 and PE1 to
CortexM7. Generated files are under `external/CubeMX/CM4`, `CM7` and `Common`.

The export retains CMake/GCC, separate peripheral `.c/.h` files, referenced
driver libraries, preserved user-code sections and `NoMain=true`. The two C++
entry points live here so regenerating CubeMX does not overwrite them. The root
presets use the generated startup/configuration/linker files and the repository's
HAL/CMSIS submodules; they do not depend on CubeMX's generated absolute driver
paths. Build through the root presets, not the generated standalone CMake files.

The runtime-based applications are described in [the board setup](../../docs/nucleo-h755ziq/README.md);
they use the separate `h755-io-*` and `h755-ui-*` presets.

Debug and Release builds of both images pass. Both Debug images have been flashed
and verified by readback on the H755. Live ST-Link checks confirmed advancing
millisecond ticks on both cores, 64 MHz clock configuration, green/yellow GPIO
transitions consistent with the intended blink periods, and successful startup
after three board resets. Both cores were in their delay loops with clear fault
status registers. Visual LED behavior and USB power-cycle startup were also
confirmed manually.
No clang-tidy or sanitizer runs are part of this initial test.

Board details: [UM2408, power supply and LEDs](../../docs/nucleo-h755ziq/um2408-stm32h7-nucleo144-boards-mb1363-stmicroelectronics.pdf).
