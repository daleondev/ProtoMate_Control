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
M1 encoder, QSPI/SD storage, perfboard cable headers and reference switches. All 52 footprints
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
The buck’s barrel jack is the 24 V system inlet. Its VIN header pins feed
separate underside power/return branches to both TMC modules and the
DM542T power terminal J112.
Connect its 5 V output to Nucleo **CN11 pin 6 (5V_EXT)** and ground to
**CN11 pin 8**; select **JP2 pins 5–6 (EXT)**. The Nucleo's 3.3 V output on
**CN8 pin 7** supplies the TMC logic, encoder buffer and storage. The board's
5V_EXT input is rated **4.75–5.25 V, 500 mA maximum**, independently of the
converter's 5 A rating. Apply external power before attaching ST-Link USB.
See [ST UM2407, sections 7.4.3 and 7.4.6](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
The buck symbol uses its marked input/output connections; wire colours are
not assumed.

The TMC2209 address straps are **M2: MS1/MS2 = GND/GND (address 0)** and
**M3: MS1/MS2 = 3.3 V/GND (address 1)**. Both use the shared USART2 bus;
firmware selects **1/16 microstepping** and verifies the settings before enabling
any driver. Leave both **SPRD jumpers open**. The M1 sheet documents the DM542T
DIP switches for the same resolution. All three 1.8° motors require
**3,200 STEP pulses per motor revolution**. UART current control bypasses the
Adafruit potentiometers; see [driver configuration](#tmc2209-uart-configuration-and-diagnostics).

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
provides a placement and hand-wiring map for one continuous **39-column × 48-row
individual-pad board with 2.54 mm pitch** (holes A1–AV39). The outer hole centres
span 96.52 × 119.38 mm. It contains six Diotec 2N2222A stages (three for
the DM542T and three for encoder voltage conversion), the AM26C32 encoder
receiver, SN74HC126N buffer, passive components and headers. The two Adafruit
TMC2209 modules mount directly at J105/J106 through soldered ten-pin control
headers and separate two-pin power headers. Their four-way motor terminals
remain on the modules. C1/C2 provide local bulk capacitance on the perfboard.
The Nucleo and DM542T remain external; J112 supplies the DM542T’s 24 V/GND
cable. The buck mounts in the lower-left area at J103, using all four
input/output pins. Its barrel jack receives 24 V. The 63 × 27 mm body follows the supplied STEP
model; the mounting uses a nominal 5.08 mm pair pitch and 50.8 mm separation.

Hand wiring uses 112 underside-only connections and 39 short top-side
crossovers through dedicated free holes; all wire ends are soldered underneath.
The assembly guide includes the soldering order and header orientation.

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

## Axis conversion

[`src/control/AxisConversion.hpp`](src/control/AxisConversion.hpp) provides
`RotaryAxisConversion` for the shoulder/elbow and `LinearAxisConversion` for Z.
They translate motor shaft coordinates into joint coordinates using
`pnm::units` angles, distances, velocities, acceleration and jerk. They contain
no hardware access or mutable motion state. `control::MotionController` owns
the motors and optional axis converters; both CLI command groups use that
same controller. `axis` operates in joint degrees/deg/s or Z mm/mm/s;
`motor` operates in shaft degrees/rpm. See the
[joint-axis CLI documentation](src/cli/README.md#joint-axis-control).

Configure a positive reduction (motor revolutions per output revolution), a
direction, and a pair of reference coordinates describing the same physical
location. For Z, also supply the travel per output pulley/screw revolution:
pulley teeth times belt pitch, or screw lead. The converter applies the
reduction to that travel. `motor_reference` must equal the reference-switch
coordinate passed to `StepperMotor`; `axis_reference` is the joint angle or
height assigned to that switch. Absolute coordinates become physically valid
after successful motor referencing. Conversion itself does not mark an axis
referenced or bypass `moveAbs()` rejection.

```cpp
#include "control/AxisConversion.hpp"
using namespace pnm::units::literals;

// Illustrative values only; the robot's reductions and joint home coordinates
// are placeholders in main.cpp, separate from the examples below.
const RotaryAxisConversion shoulder{{
    .motor_revolutions_per_axis_revolution = 5.0,
    .direction = AxisDirection::OppositeToMotor,
    .motor_reference = 135_deg, // Same value as m1's constructor argument.
    .axis_reference = 10_deg,
}};
const LinearAxisConversion vertical{{
    .travel_per_output_revolution = 40_mm,
    .motor_revolutions_per_output_revolution = 2.0,
    .direction = AxisDirection::OppositeToMotor,
    .motor_reference = 135_deg,
    .axis_reference = 200_mm,
}};

// With m1/m3 already constructed, referenced and ready for motion:
auto shoulder_move = m1.moveAbs(shoulder.toMotorPosition(28_deg),
                                shoulder.toMotorSpeed(6_rpm));
auto z_move = m3.moveRel(vertical.toMotorDisplacement(5_mm),
                         vertical.toMotorSpeed(10_mm_s));
auto shoulder_position = shoulder.toAxisPosition(m1.position());
auto z_velocity = vertical.toAxisVelocity(m3.velocity());
// Observe both futures and handle StepperMotor::Result in the controller.
```

`toAxisPosition`/`toMotorPosition` apply both the scale and reference offset.
`toAxisDisplacement`/`toMotorDisplacement` convert relative distances without
offsets. Angular positions remain unwrapped across multiple revolutions.
`toAxisVelocity`/`toMotorVelocity` preserve signed motion, including direction
inversion. `toAxisSpeed`/`toMotorSpeed` convert nonnegative magnitudes for
`moveRel`, `moveAbs`, `reference` and `setVelocity`; they reject negative
speeds. A zero magnitude converts to zero, but the motor requires positive
speeds for motion. `toAxisAcceleration`/`toMotorAcceleration` and
`toAxisJerk`/`toMotorJerk` similarly convert nonnegative profile magnitudes,
without offsets or direction inversion. Move direction follows the signed displacement or target;
referencing retains the motor's fixed Forward seek direction.

The same position/velocity conversions apply to successful encoder results;
retain errors from `actualPosition()`/`actualVelocity()` and ensure encoder
polarity matches the motor coordinate. M2/M3 use coarse electrical INDEX estimates, not shaft encoders. Reduction, travel and coordinates must be finite; invalid
configuration/inputs throw `std::invalid_argument`, and arithmetic overflow
throws `std::overflow_error`. Pulse rounding stays in `StepperMotor`. Joint
limits, coupled trajectories and SCARA forward/inverse kinematics belong above
this layer. The configuration examples do not start or synchronize any motion
in the firmware. `main.cpp` installs explicit dummy mechanics through
`MotionController::configureAxis()`: 1:1 reductions, the same direction as each
motor, zero joint coordinates at the reference switches, and 40 mm of Z travel
per output revolution. Replace these placeholders in code once actual mechanics
are known. Motor-side reference coordinates come from the existing motor
configuration. There is no interactive mechanics configuration command.

The Linux `application.control` tests cover conversions and their integration
with motor referencing, switch rejection and pulse accounting:

```sh
cmake --build --preset debug-linux --target application_control_tests
ctest --test-dir build/debug-linux -R '^application.control$' --output-on-failure
```

## SCARA kinematics

[`src/control/ScaraKinematics.hpp`](src/control/ScaraKinematics.hpp) implements
the robot's two rotary joints and vertical Z axis. It uses configured instances
with ordinary methods defined in the `.cpp`, just like the axis converters.
All calculations are independent of the HAL and motor workers.

`JointPosition` contains the shoulder angle relative to the base, the elbow
angle relative to the first arm, and upward-positive Z carriage travel.
Angles increase counterclockwise viewed from above. `CartesianPosition` is the
tool tip's XYZ in the world frame. Base translation/yaw place the robot in that
frame. The tool offset is measured from the Z attachment datum in the second
arm's frame: X forward, Y left, Z up. A negative tool Z offset places the tip
below the carriage. `tool_yaw` specifies the tool's fixed orientation relative
to that frame; it does not rotate the offset vector.

For `a = base_yaw + shoulder` and `b = a + elbow`, the forward position is:

```text
x = base.x + L1*cos(a) + (L2 + tool.x)*cos(b) - tool.y*sin(b)
y = base.y + L1*sin(a) + (L2 + tool.x)*sin(b) + tool.y*cos(b)
z = base.z + joint.z + tool.z
yaw = b + tool_yaw
```

The named parameter definitions in `docs/robot/SCARA_Robot.f3d` give
`R_Link1Length = 200 + 5 + 0 = 205 mm`,
`R_Link2Length = 200 + 50 - 26.6 = 223.4 mm`, and
`R_TCPForwardOffset = 135 mm`. These imply an effective second length of
358.4 mm for a centreline tool, including its forward offset. This is a reading
of the stored parameter expressions, not a measurement of the evaluated linked
assembly. Base/tool calibration, joint reference coordinates and actual travel
limits remain application configuration. `main.cpp` uses these nominal lengths
with zero base translation/yaw, a 135 mm forward tool offset, zero tool yaw and
no joint limits. Together with the dummy axis mechanics, these are calculation
defaults, not calibrated robot coordinates.

```cpp
#include "control/ScaraKinematics.hpp"
using namespace pnm::units::literals;

ScaraKinematics kinematics{{
    .first_arm_length = 205_mm,
    .second_arm_length = 223.4_mm,
    .tool_offset = {135_mm, 0_mm, 0_mm},
}}; // Geometry example; supply calibrated frames and mechanical limits for operation.

// Joint coordinates from the three configured AxisConversion objects:
ScaraKinematics::JointPosition current{30_deg, 60_deg, 50_mm};
auto pose = kinematics.forward(current); // Result<ToolPose>: XYZ and derived yaw.
auto target = kinematics.inverse({300_mm, 200_mm, 40_mm}, current);
if (target) {
    // Inspect target->shoulder / target->elbow / target->z without moving.
    // Robot::moveAbs combines this calculation with coordinated execution.
}
else {
    auto reason = target.error(); // Handle ScaraKinematics::Error in the controller.
}
auto tool_velocity = kinematics.forwardVelocity(current, {1_rpm, -2_rpm, 5_mm_s});
auto joint_velocity = kinematics.inverseVelocity(current, {10_mm_s, 0_mm_s, 0_mm_s});
```

- `inverse(target, reference)` preserves the reference's elbow branch and picks
  equivalent joint angles nearest its unwrapped coordinates, subject to optional
  inclusive `JointLimits`. It never silently changes branches to satisfy limits.
  An explicit third argument (`ElbowBranch::Positive` or `Negative`) selects a
  branch; this is required when the reference is singular. Selecting a different
  branch calculates an endpoint, not a safe transition path.
- With a sideways tool offset, branch/singularity calculations use the effective
  elbow-to-tip vector. Its phase is `atan2(tool.y, L2 + tool.x)` and its length is
  `hypot(L2 + tool.x, tool.y)`. The default inverse singularity threshold is
  `abs(sin(elbow + phase)) <= 1e-6`. Straight/folded workspace boundaries return
  `Singularity`; targets beyond the reachable annulus return `Unreachable`.
- `forwardVelocity` and `inverseVelocity` apply the positional Jacobian to signed
  velocities. Forward results also include derived tool yaw velocity. The robot
  has no independent yaw axis. Inverse velocity rejects singularities; speed and
  acceleration limits still need enforcement by the trajectory planner.
- Calculations return `std::expected<T, Error>` with `InvalidInput`, `Unreachable`,
  `JointLimitExceeded`, `Singularity` or `NumericOverflow`. Invalid configuration
  throws `std::invalid_argument`. Floating-point roundoff at workspace/limit
  boundaries is tolerated; unreachable points are not projected into the workspace.
- Forward feedback remains available outside joint limits and at singularities.
  Use `checkJointLimits` explicitly for state checks. Inverse position checks its
  resulting endpoint; inverse velocity checks its current joint position.
  Omitted limits mean unrestricted joint coordinates, not verified mechanical travel.

[`control::Robot`](src/control/Robot.hpp) connects this geometry to the existing
`MotionController`. `main.cpp` constructs it with the same shared controller
used by the motor and axis CLI modules. `Robot::status()` converts a fresh
controller snapshot into joint coordinates, tool pose and tool velocity. It
has no duplicate peripherals, worker or cached position. Conversions and motor
readings come from the same controller snapshot; individual readings are
sampled sequentially, not latched simultaneously in hardware.

The commanded state uses emitted-step positions and signed motor velocities.
The feedback state requires valid position and velocity results on all three
axes and preserves the failing axis/error if unavailable. M1 uses its shaft
encoder; M2/M3 use coarse driver INDEX estimates. A combined feedback tool pose
is therefore an estimate, not a fully measured physical pose. `axis status`
identifies each source and its availability. Unreferenced coordinates are explicitly identified,
and the generator's pulse-count validity remains visible.

`Robot::moveAbs`, `moveRel` and `moveJoints` execute synchronized, rest-to-rest
point-to-point motion. Cartesian targets are converted through IK; interpolation
is linear in joint coordinates, so the tool path can curve. Each moving axis
samples one shared acceleration/deceleration/jerk profile. The HAL arms their
prepared sequences against one timer origin, with absolute pulse times rounded
to 100 ns ticks; all moving axes emit their final step on the same tick.
Targets are rounded to the nearest microstep and checked against joint limits
again after rounding. Stationary axes hold their positions.

The default maximum joint speeds are 60 degrees/s for shoulder and elbow and
20 mm/s for Z. `MoveOptions::speed` scales those limits from greater than zero
through 1 (default 0.2). The profile also respects each motor's configured
acceleration, deceleration and jerk, plus the STEP rate limit. A move has a
10 ms setup lead, 5 microsecond high pulses and an optional overall timeout.
Very slow schedules exceeding the timer's roughly 53.68 s per-pulse horizon
are rejected. No straight Cartesian interpolation, collision checking, robot
command buffering or live robot speed override is implemented.

The controller reserves all three axes for a robot operation, rejects conflicting
motor/axis commands and releases ownership after completion or cancellation.
All axes must be idle and referenced before a robot move. Stopping any axis
stops the entire robot operation; a reference-switch activation while moving
towards it also stops the group. `Robot::reference()` homes Z, shoulder and elbow
in that order, using the existing seek/backoff/slow-latch sequence at 5/0.5 motor
RPM and a default 90 s overall timeout. Disabling drivers invalidates references.
Operations return an ID and a shared completion future; the controller keeps
the most recent 32 robot results separately from individual-axis commands.
Execution waits for progress/switch notifications and completion futures.

The [robot CLI](src/cli/README.md#robot-control) exposes these operations,
live state and read-only calculations:

```text
robot status
robot geometry
robot fk 30 60 50
robot ik 300 200 40 --branch positive
robot enable
robot home
robot jobs
```

FK inputs are shoulder/elbow degrees and Z millimetres. IK takes world XYZ in
millimetres and uses the current commanded joints as its seed. The default
branch is `current`; `positive` and `negative` select a branch explicitly.
FK/IK calculations start no motion. `robot moveto` and `robot move` execute
absolute and relative XYZ targets; `robot joints` executes a joint target.
`--speed` is a percentage of the joint speed limits (default 20), rather than
a Cartesian feed rate. `robot stop` cancels motion while keeping drivers enabled.
The geometry layer installs no callbacks and does not replace motor accounting.

The `application.control` tests cover analytic poses, both elbow branches,
multi-turn continuity, frame/tool offsets, workspace and joint-limit boundaries,
singularities, velocity finite differences, composition with the axis
converters, live robot state, missing/faulted feedback, coordinated profiles,
homing, command ownership, switch activation, cancellation and timeout handling.
CLI tests cover annotated commands, coordinate calculations, simulated robot
execution and shared controller state. Timer-model tests check the common start
origin, rollover, exact pulse accounting and partial arming failures.
Background on velocity singularities is available in
[Modern Robotics, section 5.3](https://modernrobotics.northwestern.edu/nu-gm-book-resource/5-3-singularities/).

## Stepper GPIO assignment

The following seven outputs on the **NUCLEO-H753ZI (MB1364)** control
one STEPPERONLINE DM542T driver with an Oriental Motor PKP245D23A2-R2FL
motor (M1) and two Adafruit TMC2209 #6121 boards (M2/M3).
CubeMX configures the STEP pins as **TIM2 output-compare outputs**, with
three independent DMA streams. The counter runs at **10 MHz (100 ns/tick)**.
The DIR pins and shared enable are ordinary push-pull GPIO outputs. All seven
use low GPIO speed (output slew rate, not pulse frequency); STEP additionally
uses internal pull-downs. `src/main.cpp` creates one `control::MotionController`,
which owns the shared generator, driver enable and three `StepperMotor` objects.
The motors claim their STEP/DIR, reference-switch and encoder resources; M1
starts encoder monitoring during construction. The controller starts the shared
timebase once, with drivers disabled and no motion or homing at startup.
It is injected into both motor and joint-axis CLI modules before application
threads start. Use `motor` or `axis` for command overviews, or `help axis move`
for joint-unit motion options; see the
[motor CLI and ownership documentation](src/cli/README.md#motor-control).

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
- `generator->startPrepared({delay1, delay2, delay3})` arms selected, already
  prepared axes against one common timer origin. `std::nullopt` leaves that
  axis untouched. Initial buffers are generated before sampling the origin;
  an arming failure stops all selected axes and retains any emitted counts.
  Robot motion uses different first-pulse delays to sample one shared profile.
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
StepperMotor m1{Motor1, 135_deg, 1.8_deg, 16U, generator};
StepperMotor m2{Motor2, 135_deg, 1.8_deg, 16U, generator};
StepperMotor m3{Motor3, 135_deg, 1.8_deg, 16U, generator};
if (!generator->start()) throw std::runtime_error("step timebase failed");

// After the controller enables the drivers and observes their settling time:
auto motion1 = m1.moveRel(90_deg, 300_rpm);
auto motion2 = m2.moveRel(10_rev, 150_rpm, 720_deg_s2, 1080_deg_s2, 7200_deg_s3);
// Once m2 is running, a velocity change returns its first affected pulse:
auto changed_at = m2.setVelocity(300_rpm); // Check the Result for rejection.
m1.stop();                              // m2 continues.
m2.stopAndWait();
auto result1 = motion1.get();
auto result2 = motion2.get();
```

`moveRel()` and `moveAbs()` accept `(position, velocity, acceleration,
deceleration, jerk, buffer_mode, timeout)`. They return one future per command.
Acceleration/deceleration use `pnm::units::AngularAcceleration` (`deg_s2`,
`rad_s2`); jerk uses `AngularJerk` (`deg_s3`, `rad_s3`). Pneumo also provides
linear `Jerk` (`mm_s3`, `m_s3`) and the corresponding time-based arithmetic.
The three dynamics arguments default to zero, meaning **use the configured
axis default**, not an instantaneous ramp. `setMotionDefaults()` configures
positive acceleration/deceleration and nonnegative jerk while idle. Initial
software defaults are 3600 °/s² acceleration/deceleration and 36000 °/s³ jerk;
these must be tuned to the actual mechanics. A configured jerk of zero selects
a trapezoidal profile. Positive jerk produces a continuous-acceleration S-curve.
Short moves automatically reduce their peak speed to fit the available distance.
Negative/nonfinite dynamics are rejected. The existing `(position, velocity,
timeout)` overload remains available.

`BufferMode` uses the following names (the equivalent PLC names have an `MC_`
prefix). There is room for one active command and one queued successor:

| Mode | Behavior when the axis is already moving |
| --- | --- |
| `Aborting` (default) | Immediately stop/join the current worker, cancel its queued successor, then start the new command from rest. |
| `Buffered` | Complete the previous move at rest, then start the successor. |
| `BlendingLow` | Request the lower of the two commanded speeds at their junction. |
| `BlendingPrevious` | Request the previous command's speed at the junction. |
| `BlendingNext` | Request the successor's speed at the junction. |
| `BlendingHigh` | Request the higher of the two commanded speeds at the junction. |

Blending joins same-direction commands within one hardware pulse stream.
Velocity and acceleration are continuous at the replanned boundary. The planner
reduces an infeasible junction speed; if the tail is already committed or no
suitable splice fits, the successor executes from rest. A direction reversal
always comes to rest. Submit successors early, before their predecessor's final
DMA buffers are filled. Relative successors are relative to the predecessor's
endpoint; absolute targets remain absolute. A second queued command returns
`Rejected` and leaves accepted commands intact. A new `Aborting` command,
`stop()`, switch activation toward the switch, timeout or fault cancels queued
work. These abort paths remain **immediate stops**, not deceleration ramps.
There is no coupled-slave axis mode in this API.

```cpp
auto first = m2.moveRel(-360_deg, 120_rpm, 1440_deg_s2, 2160_deg_s2, 14400_deg_s3);
auto next = m2.moveRel(-180_deg, 60_rpm, 1440_deg_s2, 2160_deg_s2, 14400_deg_s3,
                       StepperMotor::BufferMode::BlendingLow);
// Each future belongs to a separate submitted command; inspect both results.
```

Finite moves round to the nearest microstep. `position()` tracks signed
commanded pulses from a software zero. Per-axis progress callbacks update
`m_position` and `m_velocity`, including the final stop/completion; both getters
refresh hardware progress and return those atomic fields. `velocity()` uses the
timer-rounded interval following the latest emitted pulse and is zero before
the first pulse and after stopping. It is a discrete STEP-rate measurement,
not an exact sample of the continuous mathematical velocity.
`moveAbs()` is rejected until `isReferenced()` is true, including zero-distance
requests. Velocity arguments are positive magnitudes. Zero timeout means no
deadline; a buffered command's timeout starts when it becomes active.

`setVelocity()` asks the motion worker to replan the uncommitted tail using the
active acceleration/deceleration/jerk settings and returns the first affected
pulse number. It preserves the finite endpoint and any accepted blend endpoint;
it returns an error when the change cannot fit. This is a thread-context call,
which waits for planning to finish. It requires an active, started motion with
unbuffered pulses; calling immediately after asynchronous submission can precede
startup and be rejected. Already committed pulses remain unchanged. Following-
error handling remains future controller work. `control::MotionController`
owns shared driver enable, enforces the 200 ms settling delay before accepting
motion, and tracks bounded asynchronous command results for all clients.

The planner stores a bounded number of analytic phases, not an entry for every
pulse. The HAL accepts an immutable `hal::step::Sequence`, evaluates its timing
while refilling DMA, and supports `scheduleCursor()` / `replaceSequence()` to
replace an uncommitted tail. A cursor is revision-checked: a refill, replacement
or restart invalidates stale cursors. Providers must be allocation-free,
nonblocking and bounded in ISR context. Invalid timing stops the generator;
retired providers are released by the calling thread after the HAL unlocks.

`StepperMotor` takes the reference-switch coordinate as its second constructor
argument: `StepperMotor{Motor1, 135_deg, 1.8_deg, 16U, generator}`. The current
`main.cpp` configures 135° for each motor. Calling `reference()` performs a
switch reference sequence; it does not enable the drivers or start the shared
generator. Defaults are 5 rpm for seeking/backing off, 0.5 rpm for the second
approach, and a 30 s overall timeout. Both speeds must be positive and the second
must be slower; the timeout must be finite and greater than zero.

```cpp
auto homing = m1.reference(5_rpm, 0.5_rpm, 30_s);
if (homing.get() == StepperMotor::Result::Completed) {
    auto motion = m1.moveAbs(90_deg, 10_rpm); // Choose a target away from the switch.
    auto result = motion.get();
}
```

The sequence approaches Forward until activation, backs off Backward until
release, then approaches Forward at the second speed until activation again.
An initially active switch starts with backoff. Every contact/release stops the
axis immediately when the worker handles the event, then requires 10 ms of
stable switch level before advancing. Bounce wakes the worker and restarts that
settling interval. The timeout covers all phases, including settling; a missing
activation or release cannot leave an endless seek running.

Only successful completion sets `m_position` and the encoder coordinate to
`m_referenceSwitchPosition` and marks the axis referenced. The encoder continues
counting; a synchronized coordinate offset preserves the origin through later
callbacks without resetting the raw count or creating an artificial velocity
jump. The origin is assigned at the stopped, confirmed second contact, so the
reference accuracy includes switch repeatability and worker stopping latency.
M2/M3 rebase their coarse INDEX coordinates at the same reference position;
an input with no observed INDEX transition remains unavailable. Starting a valid reference
attempt clears the old referenced flag; timeout, cancellation or failure leaves
it clear. `stop()`/`stopAndWait()` cancel any phase, and replacing the command
stops/joins its worker. `setVelocity()` is rejected during referencing so its
configured seek speeds are preserved. Other axes continue independently.

Motion workers block on an event until their axis completes/stops/faults, a stop
request or reference-switch activation arrives, or the deadline expires. They do not poll every millisecond.
Each motor owns its axis progress subscription and a pre-created
`runtime::Notification`. The callback accounts new pulses and executed timing;
a terminal state or a reached blend boundary signals the event. Queued commands
and velocity-change requests also wake the worker. The worker stops the output when needed,
checks the final status and resolves its future in thread context. Accounting is
performed once in the callback, including the final count captured by `stop()`.
Signals arriving before a wait are retained, and repeated signals coalesce.
Cancellation and the reference-input callback also signal the event. A replacement joins the old worker before
clearing old notifications and starting its next motion. Completion notification
still uses the existing DMA/TIM7 service; STEP timing is unchanged. Progress is
batched at those service points, not an interrupt for every pulse. The fields
therefore update autonomously between reads at service cadence, and getters
refresh them to the sampled hardware count. They describe commanded motion,
not measured encoder position or rotor speed. If a DMA fault makes counts
uncertain, position retains its last exact value and the reference is invalidated.

`StepperMotor::actualPosition()` and `actualVelocity()` return encoder measurements
as `pnm::Result<Angle>` / `pnm::Result<AngularVelocity>`. M1 uses the board's
1,600 counts/revolution (0.225°/count), independently of its microstep setting.
The HAL invokes a sample callback every 10 ms, including while the shaft is
stationary. That callback updates `m_actualPosition` and `m_actualVelocity`;
getters read these atomic fields without polling hardware. Velocity is signed
count displacement divided by the actual monotonic sample interval. It is a
window average, with about 3.75 rpm per count at a 10 ms interval and the current
steady clock's 1 ms timestamp resolution. Slow rotation can therefore alternate
between zero and nonzero speed samples; this is unfiltered encoder quantization.

Counting continues when STEP is stopped, so manual shaft movement and coasting
are measured. Position starts at zero when the motor object is constructed;
a successful `reference()` anchors its coordinate to the configured switch
position. Its sign follows the A/B wiring, independently of commanded direction.
No encoder-index reset, closed-loop position correction or stall response is applied.
M2/M3 use the [callback-based driver INDEX provider](#callback-based-driver-index-feedback)
without substituting commanded steps for missing feedback. A latched shaft-encoder
count error makes both measured getters report `state_not_recoverable` and
invalidates the referenced flag.
Destruction disconnects the subscription and stops encoder counting.

`runtime/synchronization/Notification.hpp` is built by
`runtime::synchronization`. Shared ThreadX helpers live in `runtime/threadx/`;
the synchronization component has no dependency on `libstdcxx` implementation
headers. The umbrella `runtime::runtime` target links these components together.

`runtime::Notification::signal()` supports ISR and interrupt-masked callers.
On STM32 it preserves the caller's interrupt mask and defers any required
context switch until interrupts can run. Standard condition-variable or
semaphore methods in this runtime must not be called from an ISR. Notification
creation, destruction and waiting require thread context, with no producer or
waiter remaining when the object is destroyed.

Timing uses nanoseconds, rounded up to 100 ns ticks. Both high and low phases
must be at least **5 µs**, giving a configured ceiling of 100,000 steps/s per
axis; periods up to **53.6870911 s** are representable. These are software
limits, not measured electrical performance. DIR setup/hold and the DM542T's
200 ms enable delay remain the controller's responsibility. The generator does
not change DIR/enable or implement reference-switch stopping.

Each axis has two buffers of up to 512 edge timestamps (256 pulses each).
A live timing update therefore has up to **512 pulses of lookahead**: roughly
5.12 ms at 100 kHz, or 512 ms at 1 kHz. It changes the high time of the returned
pulse and its following rising-edge interval. The motor planner uses the immutable sequence API for ramps and blending.
Its first pulse includes the time to accelerate through one microstep, plus a
10 ms arming margin. Profile periods are quantized to timer ticks.

Very slow profiles use smaller blocks so the queued horizon stays below a
quarter counter cycle. Buffer length stays fixed during a motion. Live updates
must also fit that horizon: with full 512-edge blocks, periods above
**209.7151 ms** are rejected (about 4.77 steps/s minimum). More extreme slowdowns
require a new motion or a prepared sequence. Updates are rejected once all
finite pulses are buffered. `updateTiming()` accepts uniform trains only; analytic
profiles use a revision-checked replacement sequence. Very small
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

`generator->setProgressCallback()` supplies **batched cumulative progress/completion**.
It deliberately does not promise one callback per physical step: delayed
interrupts can combine progress. Position comes from hardware/DMA progress,
not from counting callbacks. A STEP-pad phase check distinguishes a pending DMA
write from a missed compare timestamp, so elapsed time cannot invent a pulse. Callbacks run in a DMA/TIM7 ISR or a caller that services the generator
(including status queries and motion operations);
keep them short, nonblocking, allocation-free, and do not mutate/destroy the
generator. Read-only queries are allowed. Register/clear callbacks while stopped.

`axis->setProgressCallback()` observes one axis independently. It receives a
synchronous start event with zero pulses, then batched progress and a final
completion/stop/fault event. `AxisStatus::period` describes the timing of the
latest emitted pulse, including queued timing changes and per-pulse sequences;
it is zero before the first pulse and after stopping. One view owns each axis's
progress subscription. Register while that axis is stopped; clearing or view
destruction synchronizes with dispatch. This callback runs before the axis's
completion callback and follows the same ISR restrictions.

`axis->setCompletionCallback()` is a separate, once-per-motion notification
for completion, stop or fault. It sends no pulse-progress or start events, and
coexists with the generator's progress callback. One output view owns the
subscription for an axis; another view cannot replace it. Register while that
axis is stopped (other axes may run); clearing and view destruction synchronize
with dispatch and release the capture. Clear it before destroying captured data.
The same ISR restrictions apply; an interrupt-safe notification is appropriate.

The Linux backend advances a logical timer/DMA simulation from a background
ThreadX worker as well as synchronous queries. Blocked motor workers therefore
receive completion events without polling. Shutdown joins the simulation worker
before releasing hardware or callback captures. Its generator lock uses the
ThreadX C API so callbacks can safely wake application threads across the
native-HAL/runtime ABI boundary. It does not generate electrical signals or
promise real-time host scheduling. The deterministic test model additionally permits
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
`setSampleCallback()` installs one subscriber and immediately reports the current
sample; `clearSampleCallback()` disconnects it and waits for any callback in
progress. Samples contain the count result, monotonic timestamp and running
state. Start/stop also publish samples. While running, the STM32 backend uses
the existing TIM6 HAL timebase interrupt to schedule samples every 10 ms,
coalescing delayed service. Callbacks must be short and nonblocking and must not
call encoder methods. TIM3 still counts edges and handles count extension;
there is no interrupt per encoder edge and STEP timing does not use this callback.

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
movement injected while stopped is ignored. A background service delivers the
same timed sample callbacks. `hal::encoder::simulatedEncoder(3)` accesses the
already owned encoder for motion injection in tests. A/B GPIO levels are not decoded
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
`IDigitalInput` for each motor. `StepperMotor` claims it during construction. Its direction
convention is **Forward toward the reference switch; Backward away**. A HIGH
input rejects toward moves with `Rejected`; away moves remain allowed. A zero
distance relative move requires no pulses and completes even with an active switch;
an absolute move additionally requires successful referencing.

A LOW-to-HIGH activation during a toward move latches a stop request and wakes
the worker, which stops that axis and returns `Stopped`. The interrupt callback
uses a lock-free flag and an ISR-safe notification; it never locks a thread mutex
or calls `StepperMotor::stop()`. Release does not stop a move or clear a pending
activation. Activations during retreat also leave retreat running. Each new move
clears the previous trip before checking the physical level; checks around
preparation/arming and the retained event prevent a brief activation being lost.
Stopping takes effect when the worker runs, so additional pulses can occur
during scheduling latency. This is not a hardware emergency stop.

The first activation is acted on without a debounce delay; contact bounce cannot
cancel an already requested stop. `reference()` additionally confirms each stopped
contact/release with a 10 ms stable-level interval. Electrical input noise filtering
remains hardware work. Route each signal with its ground return away from
motor wiring. For longer cables, add a stronger external pull-up to **3.3 V**
and input filtering as needed.

### Callback-based driver INDEX feedback

`hal::device::IndexFeedback` subscribes to the M2/M3 GPIOs from
`hal::board::createStepperIndex()`. `StepperMotor` owns one provider per TMC axis;
callbacks update `m_actualPosition` and `m_actualVelocity`. M1 keeps its TIM3
shaft encoder. `feedbackSource()` and `feedbackResolution()` identify the source;
`motor status`, `axis status` and `robot status` distinguish these estimates.

Use **normal INDEX** (`index_step=0`, `index_otpw=0`, `VACTUAL=0`). One electrical
cycle covers **four full steps: 7.2° at the motor shaft, or 64 STEP pulses at
1/16 microstepping**. The first observed boundary establishes a coarse relative
origin; subsequent crossings update position. Successful homing rebases this
coordinate to the reference-switch position. Position holds between boundaries,
with up to one electrical cycle of unresolved sub-cycle displacement/phase;
short moves can produce no position update. This is driver phase feedback and
cannot detect mechanical stalls, slipped belts or shaft motion while disabled.
See [TMC2209 datasheet, sections 13.4 and 14](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf).

Both GPIO edges are enabled: forward rising and backward falling transitions
represent the same boundary, so crossing it and reversing cancels the count.
Direction comes from the motor command. Velocity is the signed average between
same-direction boundaries over at least 10 ms (multiple cycles are accumulated
at high speed because the monotonic clock resolves 1 ms); it is zero until a complete interval is
available after startup/reversal/resume, and immediately zero on STEP stop.
Callbacks also accept the last settling INDEX edge after stopping. No UART polls
or new motion timer are involved; timer/DMA STEP scheduling stays unchanged.

Until an INDEX boundary is observed, `actualPosition()` / `actualVelocity()`
return `no_message_available`. Progress callbacks detect overdue feedback after
three electrical periods (using the slowest period since the last boundary,
with a 20 ms minimum). Lost feedback becomes unavailable until homing or driver
reinitialization; it never silently reconstructs lost cycles from commanded
steps. This is diagnostic feedback, not an automatic following-error stop.
Driver disable, reset and reconfiguration invalidate its phase. M2/M3 callbacks
are short and serialized; destruction stops motion and disconnects callbacks.

### Driver interface and shared enable

Use 3.3 V push-pull MCU outputs. Power both TMC2209 **VDD** pins from 3.3 V
and join their GND pins to controller ground. Their STEP, DIR and EN inputs
can connect directly to the assigned GPIOs; EN is active low. Hardware-timed
STEP/DIR remains the motion interface. USART2 configures the drivers and reads
diagnostics; individual DIAG inputs report electrical faults. Each normal
INDEX output also connects to its dedicated Nucleo feedback input below. See the
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

### DM542T alarm input

The DM542T V4.0 **ALM+/ALM− output conducts when healthy and opens on a
fault in its default configuration**. It reports a driver protection fault,
not its specific cause, shaft movement or lost steps. The fault output is
separate from the 5 V PUL/DIR/ENA inputs; **S2 does not set its voltage**.
See [the V4.0 manual, sections 3.2, 4.2 and 11](https://www.omc-stepperonline.com/download/DM542T_V4.0.pdf).

| Connection | Assignment |
| --- | --- |
| ALM+ | J114.1 → `M1_ALM` → J115.1 → **PF2 / CN9 pin 17 / D70** |
| ALM− | J114.2 → GND |
| Nucleo ground return | J115.2 → CN11 pin 8 / GND |
| R30 | **4.7 kΩ from M1_ALM to 3.3 V**, about 0.7 mA when conducting |
| CubeMX / HAL | GPIO input, pull-up, rising-edge EXTI2, interrupt priority 5 |

No extra transistor or optocoupler is needed for this output. Do not connect
ALM or PF2 to 5 V or 24 V. The external pull-up is on the perfboard; the MCU
pull-up also makes a disconnected Nucleo harness read high. **Low = healthy;
high = fault or open cable.** A signal short to ground is not detectable by
this interface. Use the default normally conducting alarm polarity.

`hal::board::createStepperDiagnostic(MotorId::Motor1)` creates the exclusive
input, owned by `MotionController`. Its interrupt callback latches the fault,
raises shared enable to disable all three drivers, and notifies the driver
monitor without UART, logging or thread locks. The monitor stops every motion
and invalidates all references. Startup and enable check the input level, so
an already open ALM blocks operation even without an edge. M1 remains monitored
when a TMC2209 is unavailable. Clearing ALM never resumes a motion.

Use `motor driver status m1` (or `motor driver status` for all drivers) to read
the live ALM level and retained fault latch. Correct the cause first; a DM542T
protection fault may require power cycling the driver as described in its manual.
Then run `motor reset` or `motor driver init` while disabled, explicitly enable,
and re-home. Recovery also verifies both TMC2209s. **Normal firmware now requires
the DM542T ALM connection as well as both TMC2209s.** The single-TMC bench images
only own their selected TMC DIAG input and do not require M1.

On the 39 × 48 perfboard, J114 uses C2/C3, J115 uses D5/D6, and R30 uses
B6 (+3.3 V) / B2 (ALM). All five added connections run underneath, with no
additional crossovers; see [the harness and assembly tables](hardware/assembly/README.md).

### TMC2209 UART configuration and diagnostics

For temporary wiring and an interactive image that tests **one** driver with
an unloaded motor, see the [single-TMC2209 bench test](docs/tmc2209-hardware-test.md)
and the `tmc-test-stm32` build preset. The normal robot application requires both drivers and the M1 ALM connection.
For a comparison using only STEP/DIR and hardware driver settings, use
[`tmc-standalone-test-stm32`](docs/tmc2209-standalone-test.md). That image leaves
USART2 disabled and never accesses driver registers; power-cycle the driver
and set its potentiometer current before running it.

M2 and M3 share **USART2 at 115200 baud, 8N1**, separate from the USART3 CLI.
All signals use **3.3 V logic**. The board factory creates one `IUart` transport;
`MotionController` owns and serializes the two `hal::device::Tmc2209` devices.
The normal application initializes both at boot, while the shared enable stays
HIGH. Missing/unpowered drivers leave the CLI available but block motor, axis
and robot enable/motion commands, including M1 because enable is shared.

| Signal | Nucleo contact | Perfboard J113 | Connection |
| --- | --- | --- | --- |
| USART2 TX / PD5 / AF7 | **CN9 pin 6 / D53** | 1 | R29 **1 kΩ**, then shared UART bus |
| USART2 RX / PD6 / AF7 | **CN9 pin 4 / D52** | 3 | Directly to shared UART bus |
| M2_DIAG / PD4 | **CN9 pin 8 / D54** | 5 | J105.7 / M2 DIAG; rising-edge EXTI, pull-down |
| M3_DIAG / PD3 | **CN9 pin 10 / D55** | 7 | J106.7 / M3 DIAG; rising-edge EXTI, pull-down |
| M2_INDEX / PD0 | **CN9 pin 25 / D67** | 9 | J105.8 / M2 INDEX; both-edge EXTI, pull-down |
| M3_INDEX / PD1 | **CN9 pin 27 / D66** | 11 | J106.8 / M3 INDEX; both-edge EXTI, pull-down |
| Ground | CN11 pin 8 | 2, 4, 6, 8, 10, 12 | Paired signal returns |

R29 pin 2, J113.3, J105.9 and J106.9 form the **same** `TMC_UART_RX`
bidirectional bus. Only TX passes through R29. This is ordinary full-duplex
MCU UART hardware wired to the TMC single-wire interface; firmware consumes
and validates the local TX echo before decoding each reply. RX FIFO is enabled
to retain the request echo plus reply. Transactions have a bounded timeout,
CRC and frame validation; writes verify the driver's wrapping IFCNT counter.
No motion timer, DMA stream or console peripheral is repurposed.

The address straps are M2 **0** (MS1/MS2 LOW/LOW) and M3 **1** (HIGH/LOW).
Leave **SPRD open** on both modules. Software checks the strap inputs and
chip identity. It writes digital current, microstep resolution, interpolation,
chopper settings and reply delay, verifies readable configuration registers,
and checks diagnostics. `mstep_reg_select` overrides the straps, so both
motors retain the axis configuration's **16 microsteps**. Microsteps are not
independently adjustable through the CLI because that would change position
conversion. The hardware STEP generator remains responsible for all motion.

| Setting | M2 / C17HD2024-01N | M3 / BJ42D15-26V10 |
| --- | --- | --- |
| Requested run / hold current | 650 / 650 mA RMS | 550 / 550 mA RMS |
| Nominal quantized run / hold current | 625 / 625 mA RMS | 511 / 511 mA RMS |
| CLI run-current ceiling | 700 mA RMS | 590 mA RMS |
| Initial mode | StealthChop | StealthChop |
| External microsteps / interpolation | 16 / 256 enabled | 16 / 256 enabled |

The calculation uses the Adafruit board's **0.05 Ω sense resistors** and
TMC2209's 180 mV sense range, including the internal 20 mΩ contribution.
Settings round down to the available current scale. These conservative initial
limits keep the nominal sine-wave peak below the drawings' phase-current
ratings (M2 1.0 A, M3 0.84 A); torque and temperature still need characterization
on the assembled robot. **The onboard current potentiometers are bypassed
while this UART configuration is active.** M2 hold current can be reduced;
M3 hold must equal run to preserve Z holding torque. Disabling drivers removes
holding torque. Settings are volatile; boot restores the defaults above.

```text
motor driver status
motor disable
motor driver configure m2 --run 650 --hold 500 --mode stealthchop --interpolate on
motor driver configure m3 --run 550 --mode stealthchop
motor driver status m3
motor driver init
```

Configuration and explicit recovery require disabled drivers, stop all motion,
and invalidate references. `motor driver init` reinitializes both devices and
leaves them disabled. `motor reset` resets the step timebase and performs the
same driver initialization. `motor enable` verifies both again before asserting
shared enable and waiting 200 ms. `axis` and `robot` share this controller.
StealthChop is the initial mode; it requires a requested run current of at
least 512 mA RMS for the recommended minimum IRUN scale. The 200 ms enable
delay allows its initial standstill auto-tuning before motion. SpreadCycle
remains selectable with `--mode spreadcycle`; no automatic mode switch is configured.

A monitor polls each driver's GSTAT, DRV_STATUS, IOIN, SG_RESULT and readable
configuration approximately every **100 ms plus transaction/scheduling time**.
CRC/timeout/configuration errors, a driver reset, overtemperature shutdown,
short-circuit flags or charge-pump undervoltage latch a fault, disable **all
three** drivers, stop their jobs and invalidate referencing. The DIAG ISR
immediately raises shared enable and wakes that worker; UART and logging run
only in thread context. Recovery requires correcting the cause, running
`motor driver init` while disabled, enabling explicitly and re-homing. No fault
automatically resumes motion. This software monitoring is not an emergency-stop
circuit; a UART-only fault is detected at the next successful monitor cycle or timeout.

Overtemperature prewarning is logged. Open-load and StallGuard readings are
reported for diagnosis, not used as reliable homing or position feedback.
Open-load detection depends on current/mode/motion, and StallGuard needs
mechanical characterization. CoolStep, sensorless homing, the internal velocity
generator, freewheeling and OTP programming remain disabled/unused. Physical NC
reference switches and the M1 encoder keep their existing roles.

Protocol and application tests use the Linux two-driver model, including reset,
disconnect, malformed replies, unacknowledged writes, independent addresses and
DIAG shutdown. **UART communication with the assembled Adafruit boards has not
been measured yet.** For first bring-up, wire J113 and the straps as above,
power both modules, and run `motor driver status`: both must show `ready=true`,
no latched fault, the intended microsteps/current and no error. `motor driver
init` retries after wiring/power corrections; motion remains disabled until
explicitly enabled. Use `help motor driver configure` for option details.

References: [TMC2209 register and UART specification](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf),
[Adafruit pinout](https://learn.adafruit.com/adafruit-tmc2209-stepper-motor-driver-breakout-board/pinouts),
[Adafruit board schematic / sense resistors](https://github.com/adafruit/Adafruit-TMC2209-Breakout-PCB/blob/main/Adafruit%20TMC2209%20Stepper%20Motor%20Driver.sch),
[ST UM2407 connector assignments](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).

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

`hal::initialize()` and bare encoder construction leave counting stopped.
M1's `StepperMotor` installs the measurement callback and starts the encoder;
its Z input remains available for future homing. TIM3's priority-5 interrupt is
enabled by encoder `start()` and disabled by `stop()`; its handler is project-owned.
TIM6 schedules measurement callbacks through the project-owned HAL period
callback, while still incrementing the HAL tick. The generated CubeMX callback
is renamed at build time, so regeneration preserves this integration.
EXTI9_5 retains the HAL wrapper's shared priority-5 dispatcher.

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
