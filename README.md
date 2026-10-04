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
perfboard layout. Seven sheets cover controller/power, M1/DM542T, M2/M3/TMC2209,
M1 encoder, QSPI/SD storage, perfboard cable headers and reference switches. All 44 footprints
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

The onboard **STLINK-V3 MCO must be set to HSE/5 (5 MHz)**, derived from its
25 MHz crystal. This is a persistent ST-Link setting, separate from CubeMX;
the connected board is configured accordingly. The target uses HSE bypass,
PLL1 M=1/N=192/P=2/Q=24/R=2, giving 480 MHz CPU, 240 MHz timer kernels and
the STEP counter's 10 MHz tick. See the
[clock setup procedure](docs/stepper-hardware-test.md#clock-source-configuration)
when preparing another board or changing ST-Link firmware.

`/flash` is a 12 MiB FAT volume on the 16 MiB NOR chip, with remaining capacity
reserved for LevelX reclamation. Blank media is formatted on first use; invalid
existing media is not automatically erased. `/sd` mounts the existing FAT
volume and is never automatically formatted on hardware. A 4-bit SD read CRC
failure can trigger the preserved 1-bit retry; diagnostics identify the final
bus width and error. Startup chooses `/flash`, or `/sd` if flash is unavailable,
and panics if neither mounts only when `RUNTIME_STORAGE_REQUIRED=ON`.
The virtual root is read-only; cross-volume
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
CubeMX configures the STEP pins as **TIM2 output-compare outputs**, with
three independent DMA streams. The counter runs at **10 MHz (100 ns/tick)**.
The DIR pins and shared enable are ordinary push-pull GPIO outputs. All seven
use low GPIO speed (output slew rate, not pulse frequency); STEP additionally
uses internal pull-downs. `src/main.cpp` creates one `IStepGenerator` and injects
it into three `StepperMotor` objects, which claim their STEP/DIR, reference
switch and encoder resources. Main starts the shared timebase once. All axes
remain idle, the encoder remains stopped, STEP/DIR stay low, and the shared
enable stays high (disabled). No movement or homing runs automatically.

| Signal | STM32 pin | Board connector | Driver connection | Function |
| --- | --- | --- | --- | --- |
| M1_STEP | PA0 | CN10 pin 29 / D32 | DM542T PUL− through Q1 below | TIM2_CH1 / AF1 |
| M1_DIR | PE12 | CN10 pin 26 / D39 | DM542T DIR− through Q2 below | GPIO |
| M2_STEP | PB10 | CN10 pin 32 / D36 | First TMC2209 STEP | TIM2_CH3 / AF1 |
| M2_DIR | PE13 | CN10 pin 10 / D3 | First TMC2209 DIR | GPIO |
| M3_STEP | PB11 | CN10 pin 34 / D35 | Second TMC2209 STEP | TIM2_CH4 / AF1 |
| M3_DIR | PE14 | CN10 pin 8 / D4 | Second TMC2209 DIR | GPIO |
| STEPPERS_EN_N | PE15 | CN10 pin 30 / D37 | Both TMC2209 EN; DM542T ENA− through Q3 | GPIO |

Connector positions follow [ST UM2407, table 21](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
PA0 reaches CN10.29 through **SB75 ON**. Keep the user button on PC13
(SB58 OFF). TIM2_CH2 has no external pin; PB3 remains available for SWO.
The [STM32H753 alternate-function tables](https://www.st.com/resource/en/datasheet/stm32h753zi.pdf)
confirm all three STEP routes as AF1. Existing signal-conditioning components
and perfboard nets are unchanged; use the revised J101 harness destinations.

| Timer / DMA resource | Purpose |
| --- | --- |
| TIM2_CH1 / DMA1 stream 0 | M1 edge timestamps |
| TIM2_CH3 / DMA1 stream 1 | M2 edge timestamps |
| TIM2_CH4 / DMA1 stream 2 | M3 edge timestamps |
| TIM2_CH2 / DMA1 stream 3 | Internal deadline: DMA writes CR1=0 to stop the counter |
| TIM5 | 32-bit runtime clock, moved from TIM2; `hal::timer::create(5)` |
| TIM6 | HAL timebase |
| TIM7 | Internal 1 ms finite-completion monitor; no output pin |
| TIM3, PB4/PB5 | Existing M1 quadrature encoder |
| TIM1, PE9/PE11 | Reserved for a future M2 encoder; not configured or connected |
| TIM8, PC6/PC7 | Reserved for a future M3 encoder; not configured or connected |
| TIM4 | Available |

### Hardware step generator and encoder HAL

[IStepGenerator / IStepOutput](platform/hal/drivers/itf/IStepGenerator.hpp)
replace ordinary PWM for the robot's STEP signals. Output compare toggles a
pin at each queued rising/falling timestamp. Every axis has its own timestamps,
so sharing TIM2 **does not force a shared speed or pulse count**. DMA updates
CCRx after each edge, with compare preload disabled. The CPU does not rearm
individual pulses; it fills the inactive DMA buffer while the other runs.
See the local [RM0433](docs/board/rm0433-stm32h742-stm32h743753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf),
§39.3.8 (output compare), §39.4 (timer registers), and its DMA/DMAMUX chapters.

Create one generator with `hal::board::createStepperGenerator()`, then obtain
axes with `hal::board::createStepperStepOutput(generator, MotorId::Motor1/Motor2/Motor3)`.
Views retain the shared generator. A second generator cannot claim its pins
until the first generator and all its views are released.

- `generator->start()` starts only the shared timebase. Call it once outside
  the motors; repeated calls while running do not reset it or start any axis.
- `axis->prepare({period, high_time}, count)` prepares exactly `count` pulses;
  omit `count` for continuous counted operation. Zero count is rejected.
  Only the selected axis must be stopped; other axes can keep moving.
- `axis->prepareSequence(timings)` copies one timing per pulse for a planned
  acceleration/deceleration profile. Each period starts at that pulse's rising
  edge. The controller calculates the profile.
- `axis->start(delay)` starts only that axis on the running timebase. The default
  delay is 1 ms; very short delays can return `timed_out` if setup consumes the
  scheduling margin. Each successful start resets only that axis's run count.
- `axis->updateTiming({period, high_time})` changes an active uniform train
  without restarting it. It returns the **first affected pulse number (1-based)**.
  Already buffered edges and the requested finite pulse count remain unchanged.
  A later update can replace a pending update before another buffer is filled.
- `axis->stop()` immediately aborts only that axis and drives its STEP low.
  It can shorten the final high phase. Its count survives the stop.
  `axis->clear()` discards a stopped axis's prepared motion.
- `axis->status()` reports its state, pulse count and count validity;
  `axis->pulseCount()` reports commanded rising edges, including an in-flight
  high phase or compare awaiting its DMA write. This is not encoder position.
- `generator->status()` reports the timebase state, all axis states and counts.
  **The generator stays `Running` when individual axes complete or stop.**
- `generator->stop()` shuts down the timebase and all axes. A DMA error or
  refill underrun also stops the group and latches a fault. Recover explicitly
  with `stop()` then `start()`; axes still need explicit starts afterward.

The application API follows this ownership pattern:

```cpp
using namespace pnm::units::literals;
using enum hal::board::MotorId;
auto generator = hal::board::createStepperGenerator();
if (!generator) throw std::runtime_error("step generator unavailable");
StepperMotor m1{Motor1, 1.8_deg, 16U, generator};
StepperMotor m2{Motor2, 1.8_deg, 16U, generator};
StepperMotor m3{Motor3, 1.8_deg, 16U, generator};
if (!generator->start()) throw std::runtime_error("step timebase failed");

// After the controller enables the drivers and observes their settling time:
auto motion1 = m1.moveRel(90_deg, 300_rpm);
auto motion2 = m2.move(StepperMotor::Direction::Forward, 150_rpm);
// Once m2 is running, a velocity change returns its first affected pulse:
auto changed_at = m2.setVelocity(300_rpm); // Check the Result for rejection.
m1.stop();                              // m2 continues.
m2.stopAndWait();
auto result1 = motion1.get();
auto result2 = motion2.get();
```

`move`, `moveRel` and `moveAbs` return futures. Replacing a motor's motion
stops and joins only its previous worker. Finite moves round to the nearest
microstep; `position()` tracks signed commanded pulses from a software zero,
updated by the worker and finalized on completion/join. Absolute moves do not
imply homing. Velocity arguments are positive magnitudes; direction comes from
`Direction` or the signed target distance. A zero timeout means unlimited time.
`setVelocity()` requires an active motion with unbuffered pulses; calling it
immediately after the asynchronous `move()` can precede the start and be rejected.
Encoder feedback, homing, limit-switch stopping, automatic ramp planning and
shared driver-enable policy remain controller work.

Timing uses nanoseconds, rounded up to 100 ns ticks. Both high and low phases
must be at least **5 µs**, giving a configured ceiling of 100,000 steps/s per
axis; periods up to **53.6870911 s** are representable. These are software
limits, not measured electrical performance. DIR setup/hold and the DM542T's
200 ms enable delay remain the controller's responsibility. The generator does
not change DIR/enable or implement reference-switch stopping.

Each axis has two buffers of up to 512 edge timestamps (256 pulses each).
A live timing update therefore has up to **512 pulses of lookahead**: roughly
5.12 ms at 100 kHz, or 512 ms at 1 kHz. It changes the high time of the returned
pulse and its following rising-edge interval. For precisely planned acceleration,
use `prepareSequence()` so every pulse's timing is known in advance. The motor
wrapper currently exposes uniform moves and live velocity changes; it does not
calculate acceleration profiles.

Very slow profiles use smaller blocks so the queued horizon stays below a
quarter counter cycle. Buffer length stays fixed during a motion. Live updates
must also fit that horizon: with full 512-edge blocks, periods above
**209.7151 ms** are rejected (about 4.77 steps/s minimum). More extreme slowdowns
require a new motion or a prepared sequence. Updates are rejected once all
finite pulses are buffered and for multi-pulse prepared sequences. Very small
blocks chosen for an initially slow move increase refill interrupt load when
accelerated; use a prepared sequence spanning the intended speed range for
large changes.

The counter uses `ARR=0xFFFFFFFE`; `CCR=0xFFFFFFFF`
parks a completed axis without a future output transition. Overflow may still
raise a compare flag for this value, so completion, DMA progress and wrap
handling are checked together. Finite completion includes the last full high
phase and does not depend on an interrupt arriving in time to prevent pulse N+1.

TIM2_CH2 provides an independent hardware deadline. It stops the counter if a
buffer is not replenished before its validated schedule ends. A late interrupt
cannot silently restart the timer or replay the old buffer. At an underrun,
another axis may be frozen high until the handler drives the pins low; no new
STEP edges are generated after the counter stops. The active motions are then
reported as failed. Each finite axis parks after its final falling edge while
TIM2 and other axes continue. TIM7 checks completion every 1 ms while a finite
tail is buffered; it never schedules STEP edges. The terminal guard includes
2 ms for completion service. Excessively delayed service still faults the group,
even if the last requested pulse has already finished. DMA errors also abort
the group; neither mechanism replaces an external emergency stop.

On STM32, `hal::panic()`, `Error_Handler()` and the Cortex NMI/fault handlers
stop TIM2, drive all STEP pins low and drive `STEPPERS_EN_N` high before panic
reporting. This terminal path uses direct registers without locks, allocation
or interrupt service, including before normal GPIO initialization. Custom panic
reporting hooks run after shutdown. A fault may truncate a pulse and invalidate
position; disabling the drivers also removes holding torque.

DMA1 streams 0–3 are exclusive to this engine. The 12,320-byte DMA allocation
is in `.StepDmaSection` at **0x30000000**, covered by a dedicated **16 KiB,
non-cacheable, execute-never MPU region**. Buffers are not in DTCM. CPU buffer
writes are published before extending the deadline. DMA and TIM7 interrupts use priority 5. DMA interrupts occur at buffer
half/completion boundaries, not once per pulse.

`setProgressCallback()` supplies **batched cumulative progress/completion**.
It deliberately does not promise one callback per physical step: delayed
interrupts can combine progress. Position comes from hardware/DMA progress,
not from counting callbacks. A STEP-pad phase check distinguishes a pending DMA
write from a missed compare timestamp, so elapsed time cannot invent a pulse. Callbacks run in a DMA/TIM7 ISR or a caller that services the generator
(including status queries and motion operations);
keep them short, nonblocking, allocation-free, and do not mutate/destroy the
generator. Read-only queries are allowed. Register/clear callbacks while stopped.

The Linux backend advances a logical timer/DMA simulation when queried, servicing
virtual buffer interrupts. It does not generate electrical signals or promise
real-time host scheduling. The deterministic test model additionally permits
withholding interrupts or DMA transfers to exercise failure behavior.

With GCC 16 and GoogleTest installed, run the scheduler tests independently:

```sh
cmake -S platform/hal/tests/step_model -B /tmp/protomate-step-tests
cmake --build /tmp/protomate-step-tests
ctest --test-dir /tmp/protomate-step-tests --output-on-failure
```

The suite compiles the actual STM32 register adapter against a host peripheral
model in addition to testing the common scheduler. Tests cover staggered
starts, independent stop/restart, live timing changes, finite and continuous trains,
acceleration sequences, buffer boundaries, delayed/missed interrupts, pending
DMA, wrap, abort, restart, callbacks and errors. Before operating motors, measure
all three outputs together with a logic analyzer under Ethernet/storage load,
including final pulse width, refill deadlines and stop behavior. Host models and
a firmware build do not establish DMA bus latency or transistor switching time.

For physical measurements, build the dedicated `step-test-stm32` preset.
It provides a serial menu for finite trains, independent rates, 100 kHz on
three axes, acceleration, abort, delayed interrupts, autonomous underrun stop,
independent starts/stops/live speed changes, and counter-wrap tests. Command `c` compares TIM2 against the independent
RTC crystal over three ten-second windows, with STEP held low; only UART is
needed. See the
[hardware test procedure](docs/stepper-hardware-test.md) for flashing,
analyzer connections, expected pulse counts and acceptance criteria.
The [clock and waveform measurements](docs/measurements/2026-10-03-step-generator/README.md)
document the crystal-clock configuration, three-channel logic-analyzer
captures, two-channel Hantek captures, RTC comparisons and exported figures. See the report for
measured accuracy, uncertainty and the scope of physical validation.

[IPwmOutput](platform/hal/drivers/itf/IPwmOutput.hpp) remains the general-purpose
PWM interface. Its legacy counted one-pulse mode has interrupt-rearming gaps
and is **not used by the step generator**. Generic PWM lazily initializes its
leased peripheral, so TIM1/TIM4/TIM8 are no longer boot-time motor resources.
The existing standalone `platform/hal/tests/pwm_model` tests cover that interface.

[IQuadratureEncoder](platform/hal/drivers/itf/IQuadratureEncoder.hpp) provides
`start()`, `stop()`, `isRunning()`, `position()`, `setPosition(count)` and
`reset()`. Position is a signed 64-bit count of x4 encoder edges, not motor
steps or revolutions. Start/stop preserve position; changing the origin requires
the encoder to be stopped. `position()` returns a `util::Result<Count>` so a
latched count-extension error cannot silently become a valid position. Reset
or `setPosition()` clears that error while stopped. With M1's 400 P/R encoder,
one shaft revolution corresponds to 1600 counts.

Use `hal::board::createStepperStepOutput(generator, MotorId::Motor1/Motor2/Motor3)`,
`hal::board::createStepperDirectionOutput(MotorId::Motor1/Motor2/Motor3)` and
`hal::board::createSteppersEnableOutput()` for the assigned motor outputs;
`hal::board::createEncoder(MotorId::Motor1)` creates the encoder. DIR and ENABLE
return `IDigitalOutput`: directions start low, and the single shared active-low
enable starts high (disabled). Writing enable low enables all three drivers;
writing it high disables them. The GPIO and encoder factories return exclusive, uncached
objects; repeated creation while an object is owned fails. Axis views share the
one exclusively owned step generator.
Low-level `hal::pwm::create()` and `hal::encoder::create()`
validate the supported timer/channel/pin routes. Each PWM/encoder object reserves
the whole timer and its GPIOs until destruction; unsuccessful creation releases
partial claims. The index factory returns the existing `IDigitalInput` type.
It does not reset the encoder or implement homing policy.

The Linux backend models PWM configuration and run state without generating
electrical edges or advancing with wall-clock time.
`hal::PwmOutput::advanceSimulatedPulses(count)` injects complete counted pulses;
`beginSimulatedPulse()` and `finishSimulatedPulse()` allow tests to stop during
a high phase. Callbacks run synchronously on the injecting thread. Injections
while stopped or in uncounted PWM mode are ignored. Its
`hal::QuadratureEncoder::advanceSimulatedCounts(delta)`
injects signed x4 counts, using the same count-extension arithmetic as STM32;
movement injected while stopped is ignored. A/B GPIO levels are not decoded
by this simulation. Index edges can be injected through the existing Linux
`GpioInput::setSimulatedLevel()` test interface. Simulation is not a measurement
of pulse shape, driver delays or hardware interrupt latency.

These assignments are configured in `external/CubeMX/CubeMX.ioc`.
The storage assignments above, Ethernet RMII, USART3 console,
LEDs (PB0/PE1/PB14), user button (PC13), oscillator and ST-Link debug pins stay
reserved. TIM5 supplies the runtime clock and TIM6 the HAL timebase.
TIM3 handles M1's encoder independently of the shared TIM2 STEP scheduler.

### Reference limit switches

Three Creality Ender-3 V2 mechanical switches provide one reference input per
motor. The following pins are free of the existing motor, encoder, storage and
on-board peripheral assignments. Their EXTI lines 7, 8 and 10 are distinct from
the encoder index (6) and user button (13).

| Signal | STM32 pin | Board connector | CubeMX configuration |
| --- | --- | --- | --- |
| M1_REF | PE7 | CN10 pin 20 / D41 (also CN12 pin 44) | EXTI7, rising/falling, pull-up |
| M2_REF | PE8 | CN10 pin 18 / D42 (also CN12 pin 40) | EXTI8, rising/falling, pull-up |
| M3_REF | PE10 | CN10 pin 24 / D40 (also CN12 pin 47) | EXTI10, rising/falling, pull-up |

Use **CN10 pin 22 for ground**. Connector positions follow
[ST UM2407, tables 21 and 22](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
EXTI9_5 and EXTI15_10 use priority 5 and the HAL's existing shared dispatchers.
These are digital inputs; no timer or alternate function is used.

Wire each switch's **COM and normally closed (NC) contacts between its REF
input and ground**; either contact orientation works. The pull-up is to the
Nucleo's **3.3 V**: released/closed reads `Low`, pressed/open reads `High`, and
a disconnected cable also reads `High`. The unused switch contact stays open.
The KiCad perfboard routes these signals through **J108** (Nucleo cable,
odd pins 1/3/5 = M1/M2/M3, even pins 2/4/6 = GND) and **J109/J110/J111**
(M1/M2/M3 switch cables, each pin 1 = REF, pin 2 = GND). Follow the
[harness table](hardware/assembly/Harness.csv) for cable endpoints.
The pictured board appears to have no LED, resistors or other electronics
populated, so this connection uses it as an **unpowered dry-contact switch**.
Do not connect a supply to its connector based on the `V/G/S` printing alone.
Before connecting it, use a continuity meter on the disconnected board to
identify the pair that is closed when released and opens when pressed; the
photo alone does not establish the connector contact mapping. No 5 V supply,
transistor or optocoupler is needed for this dry-contact connection.

`hal::board::createReferenceLimitSwitch(MotorId)` returns an exclusive
`IDigitalInput` for each motor. `src/main.cpp` creates and retains all three
and checks for creation failure. `read()` provides the raw level;
`setEdgeCallback()` can later report both transitions (in interrupt context
on STM32). No callbacks, debounce, homing or motor-stop behavior are installed
by the application yet. Mechanical contact bounce and cable noise must be
handled before using these inputs for motion control; route each signal with
its ground return away from motor wiring. For longer cables, add a stronger
external pull-up to **3.3 V** and input filtering as needed. These inputs do
not implement an emergency stop.

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

| PE15 / STEPPERS_EN_N | TMC2209 EN (both boards) | Q3 / DM542T ENA input current | Result |
| --- | --- | --- | --- |
| Low (0 V) | Low | Off / no current | All enabled |
| High (3.3 V) | High | On / current flowing | All disabled |

Fit an external **470 Ω pull-up to 3.3 V on PE15**, on the GPIO side of Q3's
1 kΩ base resistor. This supplies base current during MCU reset and keeps the
shared TMC2209 EN inputs high while the control supplies are present. PE15 sinks
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
