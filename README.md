# ProtoMate Control

Control software running on the robot controller of the ProtoMate SCARA Robot.

## Setup and compilers

```sh
git clone --recurse-submodules <this-repository>
# For an existing checkout:
git submodule update --init --recursive
```

Install CMake 3.22 or newer (3.24 or newer for `--fresh`), Ninja, GCC 16,
Arm GCC 16.1 or 16.2 with Newlib, OpenOCD, Arm GDB, Python 3, Git, and `patch`.
The project uses C++26 and the verified GCC 16 libstdc++ integration snapshot.
Unknown compiler majors and unverified Arm libstdc++ snapshots are rejected.
Linux tests download the pinned GoogleTest 1.17.0 source archive.

The VS Code devcontainer uses Arch Linux and checks that both compiler major
versions remain 16. Rebuild it after changing its Dockerfile. It exposes USB
and uses a development user with serial permissions. Host flashing and serial
capture also require access to the ST-Link USB and serial devices.

## Build and run

```sh
cmake --preset debug-stm32
cmake --build --preset debug-stm32
cmake --preset debug-linux
cmake --build --preset debug-linux
./build/debug-linux/Application
```

`release-stm32` and `release-linux` provide the optimized builds. After a
compiler upgrade use `cmake --preset <preset> --fresh` and rebuild cleanly.
Use `-DBUILD_TESTING=OFF` when configuring Linux to build the template without
fetching or building test dependencies.

STM32 builds produce `Application.elf`, `.hex`, `.bin`, and `.map`. Flash with:

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/debug-stm32/Application.elf verify reset exit"
```

The dual-bank target covers all 2 MiB of internal flash. The runtime test image
can exceed the first 1 MiB bank. The ST-Link USB connection also exposes COM1
(USART3) at **115200 baud, 8 data bits, no parity, 1 stop bit**, without flow
control. Prefer its stable `/dev/serial/by-id/*STLINK*if02*` path.

Linux simulates the HAL and uses the ThreadX Linux port. Standard Linux file
I/O uses the host filesystem; explicit FileX tests use simulated media images.
The simulated LED state toggles inside the process; inspect it through the
HAL or Linux debugger. Startup diagnostics and the boot message appear on the terminal.

## Storage and external wiring

Hardware startup continues without readable storage by default; file access
then fails with `ENODEV`. Full hardware conformance requires both the W25Q128 NOR module and an existing FAT SD card;
the reference SD self-test expects a card of at least 8 GiB.

| Device signal | STM32 pin |
| --- | --- |
| W25Q128 CLK | PB2 |
| W25Q128 CS | PG6 |
| W25Q128 IO0 / DI | PD11 |
| W25Q128 IO1 / DO | PD12 |
| W25Q128 IO2 / WP | PE2 |
| W25Q128 IO3 / HOLD | PD13 |
| SD D0, D1, D2, D3 | PC8, PC9, PC10, PC11 |
| SD CLK | PC12 |
| SD CMD | PD2 |
| SD card detect, advisory | PG2 |

Use 3.3 V supplies/signals and a shared ground. The CubeMX configuration is
the pin/clock source of truth. PG2 is D49 on CN8 pin 14; CN8 pin 8 is PC11,
which is already SD D3. SD data/command pins have configured pull-ups.

`/flash` is a 12 MiB FAT volume on the 16 MiB NOR chip, with remaining capacity
reserved for LevelX reclamation. Blank media is formatted on first use; invalid
existing media is not automatically erased. `/sd` mounts the existing FAT
volume and is never automatically formatted on hardware. A 4-bit SD read CRC
failure can trigger the preserved 1-bit retry; diagnostics identify the final
bus width and error. Startup chooses `/flash`, or `/sd` if flash is unavailable,
and panics if neither mounts. The virtual root is read-only; cross-volume
renames return `EXDEV`.

Successful writable-file close and filesystem metadata operations flush the
volume before returning. Explicitly close writers and check errors when
persistence matters. FAT timestamps use UTC and two-second precision.
`RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT` is destructive recovery only; all shipped
presets and the hardware runner set it to `OFF`.

## Stepper GPIO assignment

The following seven outputs on the **NUCLEO-H753ZI (MB1364)** control
one STEPPERONLINE driver (M1) and two Adafruit TMC2209 #6121 boards (M2/M3).
CubeMX configures them as push-pull outputs without internal pulls, at low GPIO
speed. `src/main.cpp` instantiates all seven through the HAL GPIO wrapper and
retains them for the application's lifetime. STEP and DIR start low; shared
enable starts high (all drivers disabled). No motion logic is implemented.

| Signal | STM32 pin | Board connector | Driver connection | Future STEP timer |
| --- | --- | --- | --- | --- |
| M1_STEP | PE9 | CN10 pin 4 / D6 | DM542T PUL− through Q1 below | TIM1_CH1 / AF1 |
| M1_DIR | PE11 | CN10 pin 6 / D5 | DM542T DIR− through Q2 below | — |
| M2_STEP | PC6 | CN7 pin 1 / D16 | First TMC2209 STEP | TIM3_CH1 / AF2 |
| M2_DIR | PC7 | CN7 pin 11 / D21 | First TMC2209 DIR | — |
| M3_STEP | PD14 | CN7 pin 16 / D10 | Second TMC2209 STEP | TIM4_CH3 / AF2 |
| M3_DIR | PD15 | CN7 pin 18 / D9 | Second TMC2209 DIR | — |
| STEPPERS_EN_N | PF3 | CN7 pin 20 / D8 | Both TMC2209 EN pins; DM542T ENA− through Q3 below | — |

Connector positions follow [ST UM2407, tables 18 and 21](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
PE9 uses the default routing to CN10 pin 4 (SB28 closed, SB70 open).
The STEP pins support three separate timers for independent pulse rates; see
the [STM32H753 alternate-function tables](https://www.st.com/resource/en/datasheet/stm32h753zi.pdf).
All seven pins currently use GPIO mode; the timer alternatives are reserved for
future pulse generation.

These assignments are configured in `external/CubeMX/CubeMX.ioc`.
The storage assignments above, Ethernet RMII, USART3 console,
LEDs (PB0/PE1/PB14), user button (PC13), oscillator and ST-Link debug pins stay
reserved. TIM2 remains assigned to the runtime timer and TIM6 to the HAL timebase.

### Driver interface and shared enable

Use 3.3 V push-pull MCU outputs. Power both TMC2209 **VDD** pins from 3.3 V
and join their GND pins to controller ground. Their STEP, DIR and EN inputs
can connect directly to the assigned GPIOs; EN is active low. Use standalone
STEP/DIR mode with hardware current/microstep settings; UART, DIAG and INDEX
need no GPIOs for this assignment. See the
[Adafruit #6121 pinout](https://learn.adafruit.com/adafruit-tmc2209-stepper-motor-driver-breakout-board/pinouts).

The STEPPERONLINE driver is **DM542T V4.0**, with S2 set to **5 V**.
Use three **2N2222A NPN transistors** as a common-anode interface for its
7–16 mA optoisolated inputs. Each GPIO drives a transistor base through **1 kΩ**;
each emitter connects to controller ground, with **10 kΩ between base and
emitter**. Q1's collector connects to PUL−, Q2's to DIR−, and Q3's to ENA−.
Connect PUL+, DIR+ and ENA+ to regulated **+5 V**, sharing the supply ground
with the controller. Check the transistor manufacturer's lead arrangement.
See the [2N2222A datasheet](https://www.st.com/resource/en/datasheet/2n2222a.pdf)
and [DM542T V4.0 manual, section 3.1](https://www.omc-stepperonline.com/download/DM542T_V4.0.pdf).

One shared enable GPIO therefore controls all three drivers:

| PF3 / STEPPERS_EN_N | TMC2209 EN (both boards) | Q3 / DM542T ENA input current | Result |
| --- | --- | --- | --- |
| Low (0 V) | Low | Off / no current | All enabled |
| High (3.3 V) | High | On / current flowing | All disabled |

Fit an external **470 Ω pull-up to 3.3 V on PF3**, on the GPIO side of Q3's
1 kΩ base resistor. This supplies base current during MCU reset and keeps the
shared TMC2209 EN inputs high while the control supplies are present. PF3 sinks
about 7 mA when driven low. The weaker 10 kΩ pull-up used for a MOSFET or logic
buffer interface is unsuitable for this shared NPN circuit. Fit external
10 kΩ pull-downs on the directly connected TMC2209 STEP inputs as well.
Keep DM542T ENA connected: an open enable input leaves that driver enabled.
For V4.0, allow at least 200 ms after enabling before issuing motion commands,
as specified in its manual.
