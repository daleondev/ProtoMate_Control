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

## KiCad hardware project

Open **[hardware/ProtoMate.kicad_pro](hardware/ProtoMate.kicad_pro)** in
**KiCad 10**. One project contains the complete system schematic and its linked
perfboard layout. Six sheets cover controller/power, M1/DM542T, M2/M3/TMC2209,
M1 encoder, QSPI/SD storage and the perfboard cable headers. All 40 footprints
are linked to schematic symbols; external equipment is marked **Exclude from
board**. Symbols and footprints use project-local libraries. See the
[hardware guide](hardware/README.md) for the structure and F8 update workflow.

For a printable view, use [the complete schematic PDF](hardware/exports/Wiring.pdf).
[System_Parts.csv](hardware/assembly/System_Parts.csv) lists all components and
identifies whether they mount on the perfboard or externally.
`DNP` means a component is shown but is not fitted. Capacitors and
pull-ups already present on storage modules need not be duplicated, as noted
on the storage sheet. Purchased modules are represented by their external
contacts, rather than reproducing their internal circuits.

The supply arrangement is **24 V DC input**, with the user's **QIQIAZI
24/12 V-to-5 V, 5 A buck converter** generating the logic/encoder supply.
Connect its 5 V output to Nucleo **CN11 pin 6 (5V_EXT)** and ground to
**CN11 pin 8**; select **JP2 pins 5–6 (EXT)**. The Nucleo's 3.3 V output on
**CN8 pin 7** supplies the TMC logic, encoder buffer and storage. The board's
5V_EXT input is rated **4.75–5.25 V, 500 mA maximum**, independently of the
converter's 5 A rating. Apply external power before attaching ST-Link USB.
See [ST UM2407, sections 7.4.3 and 7.4.6](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
The buck symbol uses its marked input/output connections; wire colours are
not assumed.

Both TMC2209 boards have **MS1 and MS2 connected to their 3.3 V VDD** for
**1/16 microstepping**. The M1 sheet documents the DM542T switches for the
same resolution. All three 1.8° motors then require **3,200 STEP pulses per
motor revolution**. Driver current settings must suit each motor; these
hardware settings do not start the stopped timers in the firmware.

M2's connector assignment was confirmed by the user to match M3: winding
pairs **1–4 and 3–6**, with **2 and 5 unused**. M1's motor and encoder
connections follow the drawings in `docs/` and the Oriental Motor manual.
The 2N2222A symbols deliberately identify **B/C/E**, since package lead order
depends on the actual transistor manufacturer. Encoder termination resistors
are marked DNP; select termination for the cable and the encoder's output
current limit before fitting them.

The connector assignments were checked against the firmware, ST's board
manual, the motor drawings, and
[Adafruit's published breakout schematic](https://github.com/adafruit/Adafruit-TMC2209-Breakout-PCB/blob/main/Adafruit%20TMC2209%20Stepper%20Motor%20Driver.sch).
KiCad's electrical-rules and layout checks, including schematic-to-board parity,
report no violations. Results are in [ERC.rpt](hardware/exports/ERC.rpt) and
[DRC.rpt](hardware/exports/DRC.rpt). To repeat the checks from the repository root:

```sh
python3 hardware/tools/refresh.py --check-only
```

## KiCad perfboard layout

The same project's [perfboard layout](hardware/ProtoMate.kicad_pcb)
provides a placement and hand-wiring map for a **100 × 160 mm individual-pad
board with 2.54 mm pitch**. It contains six Diotec 2N2222A stages (three for
the DM542T and three for encoder voltage conversion), the AM26C32 encoder
receiver, SN74HC126N buffer, passive components and cable
headers. Motor drivers, the Nucleo and the power converter connect externally.

Use [Assembly.pdf](hardware/exports/Assembly.pdf) for placement, top jumpers
and the mirrored solder-side view. The
[assembly instructions](hardware/assembly/README.md) include orientation,
parts, hole coordinates and the complete wire/harness tables. U3 is the user's
**SN74HC126N in a DIP-14 socket**, with 7.62 mm between rows.
The KiCad PCB file represents hand wiring on the purchased board; no
fabrication outputs are supplied.

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
one STEPPERONLINE DM542T driver with an Oriental Motor PKP245D23A2-R2FL
motor (M1) and two Adafruit TMC2209 #6121 boards (M2/M3).
CubeMX configures **all three STEP pins as hardware timer PWM outputs**.
The three DIR pins and shared ENABLE remain ordinary push-pull GPIO outputs.
All seven use no internal pulls and low GPIO speed. `src/main.cpp` retains three
`IPwmOutput` objects from the board factory, plus ordinary GPIO wrappers for
DIR and shared ENABLE. DIR starts low; shared enable starts high (all drivers
disabled). All three pulse timers are initialized but **stopped**, with no
requested waveform. The PWM drivers hold STEP at push-pull low while stopped
and select the timer alternate function when started. The external input
biasing below also holds STEP inactive during reset. No motion logic is implemented.

| Signal | STM32 pin | Board connector | Driver connection | STEP timer |
| --- | --- | --- | --- | --- |
| M1_STEP | PE9 | CN10 pin 4 / D6 | DM542T PUL− through Q1 below | TIM1_CH1 / AF1 |
| M1_DIR | PE11 | CN10 pin 6 / D5 | DM542T DIR− through Q2 below | — |
| M2_STEP | PD14 | CN7 pin 16 / D10 | First TMC2209 STEP | TIM4_CH3 / AF2 |
| M2_DIR | PD15 | CN7 pin 18 / D9 | First TMC2209 DIR | — |
| M3_STEP | PC6 | CN7 pin 1 / D16 | Second TMC2209 STEP | TIM8_CH1 / AF3 |
| M3_DIR | PC7 | CN7 pin 11 / D21 | Second TMC2209 DIR | — |
| STEPPERS_EN_N | PF3 | CN7 pin 20 / D8 | Both TMC2209 EN pins; DM542T ENA− through Q3 below | — |

Connector positions follow [ST UM2407, tables 18 and 21](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
PE9 uses the default routing to CN10 pin 4 (SB28 closed, SB70 open).
The STEP pins use three separate timers for independent pulse rates; see
the [STM32H753 alternate-function tables](https://www.st.com/resource/en/datasheet/stm32h753zi.pdf).
TIM1, TIM4 and TIM8 are configured identically for STEP generation: active-high
PWM mode 1, prescaler 239, period 65535 and pulse width 0. At the current
240 MHz timer clocks, each counter ticks at **1 MHz (1 µs per tick)**.
Auto-reload and compare preload are enabled. These CubeMX values are inactive
defaults; `IPwmOutput` selects a prescaler and period for the requested timing
when explicitly started.

Software will calculate movement and program timer periods/pulse widths;
**the timers generate STEP edges in hardware**, without software GPIO toggling.
The frequency is `timer_input_hz / ((PSC + 1) * (ARR + 1))`; high time is
`CCR * (PSC + 1) / timer_input_hz`. The HAL manages these registers. Motion
code still needs to respect driver pulse timing and coordinate/count steps.
The application does not call PWM start, start DMA or enable STEP interrupts.

### PWM and encoder HAL interfaces

[IPwmOutput](platform/hal/drivers/itf/IPwmOutput.hpp) provides
`configure({period, high_time})`, `timing()`, `start()`, `stop()` and
`isRunning()`. Durations use `std::chrono::nanoseconds`; microseconds and other
exactly convertible durations can be passed directly. `timing()` returns the
achievable timing, rounded up to whole nanoseconds. The 16-bit prescaler and
compare range limit representable periods; invalid requests return an error
and preserve the previous configuration. Zero and 100% duty are supported.
A request strictly between them must retain both high and low phases after
quantization. PWM construction is stopped, and starting before configuration fails.

Configuration is allowed **only while stopped**. Starting is idempotent and
arms the pulse width at an update boundary after connecting the inactive timer
output to the pin. The first high phase is complete; startup includes a low
arming interval. `stop()` immediately drives the pin low and can shorten the
last pulse. This API provides continuous PWM, not an exact finite pulse count,
acceleration, coordinated motion or live frequency changes.

[IQuadratureEncoder](platform/hal/drivers/itf/IQuadratureEncoder.hpp) provides
`start()`, `stop()`, `isRunning()`, `position()`, `setPosition(count)` and
`reset()`. Position is a signed 64-bit count of x4 encoder edges, not motor
steps or revolutions. Start/stop preserve position; changing the origin requires
the encoder to be stopped. `position()` returns a `util::Result<Count>` so a
latched count-extension error cannot silently become a valid position. Reset
or `setPosition()` clears that error while stopped. With M1's 400 P/R encoder,
one shaft revolution corresponds to 1600 counts.

Use `hal::board::createStepperStepOutput(MotorId::M1/M2/M3)` and
`hal::board::createEncoder(MotorId::M1)` for the assigned hardware. These board
factories return exclusive, uncached objects; repeated creation while an object
is owned fails. Low-level `hal::pwm::create()` and `hal::encoder::create()`
validate the supported timer/channel/pin routes. Each object reserves the
whole timer and its GPIOs until destruction; unsuccessful creation releases
partial claims. The index factory returns the existing `IDigitalInput` type.
It does not reset the encoder or implement homing policy.

The Linux backend models PWM configuration and run state without generating
electrical edges. Its `hal::QuadratureEncoder::advanceSimulatedCounts(delta)`
injects signed x4 counts, using the same count-extension arithmetic as STM32;
movement injected while stopped is ignored. A/B GPIO levels are not decoded
by this simulation. Index edges can be injected through the existing Linux
`GpioInput::setSimulatedLevel()` test interface. Simulation is not a measurement
of pulse shape, driver delays or hardware interrupt latency.

These assignments are configured in `external/CubeMX/CubeMX.ioc`.
The storage assignments above, Ethernet RMII, USART3 console,
LEDs (PB0/PE1/PB14), user button (PC13), oscillator and ST-Link debug pins stay
reserved. TIM2 remains assigned to the runtime timer and TIM6 to the HAL timebase.
TIM3 is assigned to M1's encoder below, separate from the three STEP timers.

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

### Motor 1 encoder

The **PKP245D23A2-R2FL** has a **400 P/R incremental differential A/B/Z
encoder**, powered from **5 V**. A and B are offset by a quarter cycle
(quadrature): their transition order identifies direction, and counting all
four transitions gives **1,600 counts/revolution**, or **0.225° per count** at
the motor shaft. Z is the once-per-revolution index. This is count resolution,
not guaranteed positioning accuracy; homing is still needed after power-up.
See the [motor specifications](https://catalog.orientalmotor.com/item/all-categories-legacy-products/legacy-pkp-series-2-phase-bipolar-stepper-motors/pkp245d23a2-r2fl)
and [encoder wiring, manual section 6-3](https://www.orientalmotor.com/products/pdfs/opmanuals/HM-7475-4E.pdf).

| Signal after receiver and 3.3 V buffer | STM32 pin | Board connector | CubeMX configuration |
| --- | --- | --- | --- |
| M1_ENC_A | PB4 | CN7 pin 19 / D25 | TIM3_CH1 / AF2, no pull |
| M1_ENC_B | PB5 | CN7 pin 13 / D22 | TIM3_CH2 / AF2, no pull |
| M1_ENC_Z | PB6 | CN12 pin 17 | EXTI6 rising edge, no pull |

PB4 is used for the encoder instead of JTAG reset; normal ST-Link **SWD**
debugging remains available. Connector positions follow
[ST UM2407, tables 18 and 22](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).

CubeMX configures TIM3 in **encoder mode TI1 and TI2 (x4)**, with prescaler 0,
period 65535, direct non-inverted inputs and input filters disabled. The
16-bit counter wraps at 65536. The encoder HAL extends it using signed
differences between counter samples. TIM3 update interrupts and internal CH3/CH4
compare markers at `0x5555` and `0xAAAA` service the counter even without
application reads. CH3/CH4 do not drive pins; A/B edges remain hardware-counted.
This also handles direction reversals around a wrap without guessing from the
instantaneous direction bit. **TIM3 interrupt latency must stay below the time
for 10,922 encoder counts**, keeping samples less than 32,768 counts apart.
For example, at 160,000 counts/s the latency budget is less than 68.26 ms.
Multiple unserviced wraps cannot be recovered from a 16-bit counter. The driver
reports an exactly ambiguous half-range sample or signed-position overflow;
other excessive-latency aliasing cannot always be detected.

`hal::initialize()` and construction leave counting stopped. `src/main.cpp`
creates one encoder object for A/B and a rising-edge input for Z. TIM3's
priority-5 interrupt is enabled only by encoder `start()` and disabled by
`stop()`; its handler is project-owned. EXTI9_5 retains the HAL wrapper's shared
priority-5 dispatcher. No encoder start/read loop, index callback, homing or
motion-control logic runs in the application.

Use three channels of the **AM26C32CN** differential receiver, three additional
**Diotec 2N2222A converters (Q4–Q6)**, then three channels of the user's
**SN74HC126N powered from 3.3 V**. Each transistor converts a receiver output
to 3.3 V logic and inverts it. **Reverse each encoder pair at the AM26C32
inputs as shown below** to cancel that inversion. The motor connector pinout,
MCU pins, A/B counting direction and rising-edge Z convention stay unchanged.

| Encoder + / − wires | AM26C32CN pins for those wires (reversed) | Receiver output | Converter | SN74HC126N input → output | STM32 |
| --- | --- | --- | --- | --- | --- |
| A+ / A− | 1 / 2 | 3 | Q4 | 2 → 3 | PB4 |
| B+ / B− | 7 / 6 | 5 | Q5 | 5 → 6 | PB5 |
| Z+ / Z− | 9 / 10 | 11 | Q6 | 9 → 8 | PB6 |

Each converter uses the following connections:

| Function | A channel | B channel | Z channel | Connection |
| --- | --- | --- | --- | --- |
| Base resistor, **4.7 kΩ** | R20 | R23 | R26 | Receiver output → transistor base |
| Base-to-emitter resistor, **10 kΩ** | R21 | R24 | R27 | Base → ground |
| Collector pull-up, **1 kΩ** | R22 | R25 | R28 | 3.3 V → collector |
| Transistor | Q4 | Q5 | Q6 | Emitter → ground; collector → HC126 input |

Diotec's 2N2222A lead order is **1 = E, 2 = B, 3 = C**; the schematic uses
functional E/B/C pin names. At nominal supply voltage each conducting
collector sinks about 3.3 mA. The HC126 inputs therefore receive either a
low collector voltage or the 3.3 V pull-up voltage, without a direct 5 V input.

AM26C32CN: pin 16 to regulated **5 V (±5%)**, pin 8 to ground, pin 4 to 5 V,
pin 12 to ground. Its unused fourth receiver may remain unconnected.
SN74HC126N (DIP-14): pin 14 to **3.3 V**, pin 7 to ground;
enable the three used channels by tying pins **1, 4 and 10 to 3.3 V**.
Disable its unused channel by tying **pin 13 to ground**, tie input pin 12 to ground and leave
output pin 11 unconnected. Place **100 nF** at each IC's supply pins and
share encoder, receiver, transistors, buffer and controller ground. **Never
connect the 5 V receiver outputs directly to HC126 inputs.** No SOIC adapter
or 4N35 optocouplers are used. Q1–Q3 retain their existing DM542T function.
See the [AM26C32 datasheet](https://www.ti.com/lit/ds/symlink/am26c32.pdf)
and [SN74HC126 datasheet](https://www.ti.com/lit/ds/symlink/sn74hc126.pdf), and
the [Diotec transistor pinout](https://diotec.com/tl_files/diotec/files/pdf/datasheets/2n2222a.pdf).

Keep each collector-to-HC126 connection short and local to the perfboard.
The 1 kΩ pull-up gives an estimated 10–90% rise time of 220 ns at 100 pF
total collector-node capacitance (2.2 × R × C). Transistor storage adds
turn-off delay that is not specified in Diotec's data sheet: check all three
waveforms with an oscilloscope at the intended speed before relying on the
encoder for motion feedback. The static KiCad checks do not establish a
maximum encoder frequency. Firmware and CubeMX settings are unchanged.

On the motor's eight-pin encoder connector, pin 8 is +5 V, pin 1 is ground,
and pins 2/3, 4/5 and 6/7 are A+/A−, B+/B− and Z+/Z− respectively.
Use the connector orientation shown in the motor manual. Use shielded twisted
pairs away from motor power wiring; choose receiver-end termination to suit
the cable impedance and the encoder's 20 mA output-current limit.
