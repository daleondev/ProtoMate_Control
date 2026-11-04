# Native Linux hardware simulator

```sh
cmake --preset debug-linux
cmake --build --preset debug-linux -j4
./build/debug-linux/Application
```

`Application` is the only project executable needed for deployment. It forks
before starting threads or SDL. The parent runs the native simulation and robot
viewer; the child executes the embedded controller image from a sealed Linux
`memfd`, retaining its own ThreadX C++ runtime and TouchGFX window. Nothing is
extracted to disk, and no sibling controller executable is required. The normal
system shared libraries are still required.

The build links the controller privately at
`CMakeFiles/controller/controller.elf` and embeds it as read-only data in
`Application` using C++26 `#embed` in `ControllerImage.cpp`. It is a build
intermediate, not a second deployment artifact. Models and standalone GLSL
files are embedded by `assets/RobotModels.cpp` and `assets/Shaders.cpp`; all
asset views refer to immutable static storage. No assembly embedding templates
or generated C++ byte lists are used. GCC's dependency files track embedded
model/shader inputs, and CMake also explicitly tracks the generated controller
ELF. Editing an asset rebuilds its embedding translation unit. Shader sources
are passed to OpenGL with explicit lengths, without assuming NUL termination.
The controller must remain a separate link image because its ThreadX threading
ABI and libc hooks cannot be mixed with the native C++ runtime. This packaging
preserves process isolation without loading both runtimes into one address space.

The parent reports child startup/exec errors immediately, propagates its exit
status after reaping it (even when IPC closes first), and bounds shutdown.
`ControllerProcess` owns fork/exec/reaping; `Session` owns IPC and the worker.
Typed `SessionOptions` supplies the media path, trace path and one immutable
physical profile shared with the viewer. Simulator options are not ambient
process environment variables. The child also receives SIGKILL if the parent
dies unexpectedly. Linux must permit executable `memfd` images.

The controller still owns its TouchGFX window. The viewer uses SDL2, OpenGL 4.5 core,
GLSL 450, epoxy, GLM and Assimp; it shares no renderer, shaders, widgets or graphics
resources with TouchGFX. Closing the robot window leaves simulation running.
EOF in the controller terminal requests normal application shutdown. SIGINT or
SIGTERM stops the simulator and its child. The embedded controller requires its
inherited simulator session; there is no automatic mock-hardware fallback.

Assimp is found with `find_package(assimp CONFIG REQUIRED)` and linked through
`assimp::assimp`, using the system installation like GLM/epoxy. It is required
for Linux configuration, including headless runs of this same binary, and is
absent from the embedded controller and GIGA builds. Nothing is fetched by CMake.

Three embedded dummy GLB models now pass through Assimp and follow shoulder,
elbow and Z-carriage state. The window labels them **DUMMY MODELS**. Exported
CAD can replace them with `--robot-models DIR`; see
[asset frames, export conventions and limitations](assets/robot/README.md).

Options:

- `--headless`: omit the robot window; the controller display is independent.
- `--media-dir DIR`: retain isolated `flash.img` and `sd.img`; otherwise use
  temporary, unlinked files. Existing images are size-checked and exclusively
  locked. Host `/flash` files are not used as simulated raw media.
- `--trace FILE.csv`: write timestamped GPIO transitions, including STEP, DIR,
  enable and reference contacts. The file is replaced. Capture buffering is
  bounded; overflow fails the session instead of silently losing edges.
- `--robot-models DIR`: load `shoulder`, `forearm`, `carriage` mesh files from DIR.
- `--model-scale N`, `--model-up y|z`: millimetres per imported unit and up axis
  for external models; defaults 1000 and Y (glTF convention).
- `--model-shoulder-height MM`: visual shoulder datum, default 140 mm.
- `--`: pass remaining arguments to the controller.

Orbit with left drag, pan with right drag, zoom with the wheel, reset the camera
with R. The overlay reports physical shaft positions, converted joints, TCP,
reference contacts, power, enable, faults, device time and average execution
speed. Stale snapshots are labelled and highlighted. Rendering never advances
the machine clock or performs HAL calls.

## Physical profile and scope

`Profile.hpp::Profile` defines **cad-provisional-v1**, not a calibrated robot.
Its arm lengths are 205 and 223.4 mm, with a 135 mm distal tool offset. Rotary
gearing is provisionally 6:1 and Z travel 8 mm/revolution. Shaft reference
positions are 135 degrees; corresponding joint references are 0 rad, 1.2 rad,
and 30 mm. Switches activate at 135 and release at 134.55 shaft degrees. These
values are explicit simulation fixtures, not permission to commission hardware.

The first two joints retain unwrapped coordinates. Geometry is computed from
native shaft truth. Coordinate resets, controller homing and encoder resets do
not teleport the mechanism. Mechanics are ideal kinematic tracking of accepted
pulses. Disable freezes position. There is **no inertia, gravity, payload,
contact, collision, friction, compliance, backlash or thermal model**. Physical
travel is not clamped to conceal controller errors.

Wiring follows the current GIGA board factory: STEP PA0/PA2/PA3, DIR PJ8/PD13/PH13,
shared active-low enable PC7, normally-closed references PE4/PE5/PB2, alarms
PB1/PC3/PC0, M2/M3 INDEX PB8/PB9, and M1 encoder PB4/PA7/PG7. M1 is a DM542T at
16 microsteps; M2/M3 are addressed TMC2209 devices, configured by the actual
controller driver. M1 encoder resolution is 400 P/R decoded x4. Its four-count
Z width is provisional. INDEX derives from driver electrical phase and is not
mechanical position feedback.

## Process and time boundaries

The controller child owns production device behavior, control, scheduling,
planning, FileX/LevelX, CLI and TouchGFX. The native parent owns the timer/DMA register model,
GPIO levels, motor-driver registers, encoder count, shaft truth, raw media files
and native rendering. Legacy STEP/TMC emulators are isolated test fixtures and
are not linked into the production controller executable.

A dedicated instance of the existing ICC bridge carries simulator control and
status over three bounded shared-memory lanes. It has its own bus and session,
separate from UI/IO ICC. A distinct synchronous register mailbox handles HAL
access while ThreadX interrupts are masked. It cannot depend on a coroutine
worker being scheduled. All wire packets use explicit little-endian fields,
version/session validation, checksums and bounded payloads. Shared DMA slots
are generation-checked and active banks cannot be overwritten.

One native worker owns all mutable machine state. Register transactions freeze
device time; callbacks run in the ThreadX interrupt context. Callback writes
use a fixed batch and are drained after callbacks return, before time advances.
The clock steps to the next device event, including timer rollover, DMA bank
transitions, PWM transitions and feedback edges; 1 ms clock ticks feed ThreadX.
It is paced against host time, slowing down under host load rather than dropping
hardware edges. It is not a cycle-accurate Cortex-M scheduler or a hardware
refill-deadline certification tool. Hardware qualification remains separate.

Host-monotonic heartbeats operate independently of device time and detect a
stopped peer even during a time lease. Disconnect stops native STEP output and
disables the ideal plant. Neither process automatically replays uncertain
motion or reconnects to an old session. The viewer receives immutable snapshots
at up to 100 Hz and renders at approximately 60 Hz.

## Device fidelity

- STEP uses the existing 100 ns tick model, compare toggles, 32-bit timer wrap,
  double-buffer DMA, remaining counts, bank ownership and interrupt delivery.
  GPIO, PWM and coordinated STEP share one electrical signal path and trace.
  Signed pulses drive electrical phase; mechanics advance at the valid falling
  edge. A shortened pulse can be rejected without inventing a driver alarm.
  The conservative simulation pulse constraints are 2.5 us high and 5 us DIR
  setup/hold. `sim status` reports rejected pulses separately.
- TMC emulation implements addressed reads/writes, CRC, IFCNT, reset registers,
  GSTAT write-one-to-clear, selected writable registers, microstep resolution,
  INDEX, faults and disconnects. The UART exchange boundary includes nominal
  115200-baud transfer/turnaround time. This is not an analog single-wire bus,
  UART shift-register, DMA or electrical echo-collision model.
- Reference switches have hysteresis. Encoder counters can run, stop and rebase
  independently of mechanical truth. Feedback disconnect, missed motor motion
  and shaft slip are distinct faults.
- Native general timers and PWM advance only with device time. Counted PWM
  ends low; normal PWM remains uncounted. RTC follows the session epoch plus
  device time. The standard-library wall clock remains a host service.
- Raw storage provides 16 MiB NOR and 64 MiB SD images; production FileX/LevelX
  still format, mount and access them when its runtime is initialized. The Linux
  CLI retains its existing host-filesystem behavior; the storage qualification
  probe explicitly uses the production FileX syscall adapter. NOR rejects zero-to-one programming.
  Media models do not claim SD command-level, wear, power-cut atomicity or
  flash erase/program latency fidelity.
- Host display/input, console, OS entropy and explicitly selected real host
  Ethernet adapters remain host services. Unimplemented SPI devices remain
  unavailable; they do not return fabricated successful transactions.

## Controls and verification

```text
motor driver init
motor enable
motor move m1 9 --speed 10
motor jobs
sim status
motor home m1 --seek 30 --latch 3
motor disable
```

`sim status` reports native truth independently of `motor status`. `sim power
on|off` controls the simulated motor supply only; MCU timers, GPIO enable and
driver logic remain live. A pulse interrupted by power loss is discarded. `sim fault m1|m2|m3 NAME` selects one
fault for that axis, replacing its previous fault:

- `clear`, `alarm`, `reference-stuck`, `reference-open`, `lost-step`;
- `encoder-open` (M1 only);
- `slip DEGREES` (one-time signed physical displacement);
- `dma-delay`, `irq-delay` (held until cleared);
- `uart-open`, `uart-corrupt` (M2/M3 only).

The `lost-step` fault preserves electrical pulse counts and INDEX phase while
preventing shaft movement. It is distinct from disconnecting STEP. Invalid
fault requests preserve the existing fault and pose.

Clearing an injected fault does not reset controller fault latches or re-enable
motors. Recover explicitly through the existing controller commands. Reference
open and stuck-active are electrically indistinguishable to this NC input.

```sh
ctest --test-dir build/debug-linux --output-on-failure
cmake --preset sanitize-linux
cmake --build --preset sanitize-linux --target protomate_simulation_tests
ctest --test-dir build/sanitize-linux -R '^simulation.machine$' --output-on-failure
```

`simulation.machine` exercises wire validation, interprocess queues, timing,
pulses, reference hysteresis, encoder truth, TMC registers and NOR behavior.
`simulation.integration` launches the real controller, exercises all three
motors, slip feedback, homing, alarms, recovery, stop and shutdown, and checks
recorded STEP edges. `simulation.opengl` uses Xvfb/Mesa where installed to test
actual shader execution, pixel readback, move-only resource ownership and FBO
resize/error handling. All original HAL, application, GUI and runtime tests
remain available. `simulation.lifecycle` kills/stalls peers and checks child
cleanup. `simulation.storage` writes files through FileX/LevelX, restarts both
processes, and reads them from the retained native images. GIGA builds exclude
the native simulator.

`simulation.packaging` copies only `Application` to an empty directory, starts
it, reads native hardware state through the controller, verifies exactly one
controller child executing from memory, and checks shutdown without extracted
files. `simulation.launch` checks image seals and failure/reaping behavior for
invalid embedded images.

The architecture test inspects both real link images to prevent ThreadX/TouchGFX
from entering the native process and simulated hardware from entering the
controller. Additional regressions exercise early IPC close with delayed or
stalled child exit, invalid session descriptors, a delayed initial heartbeat,
corrupt queues, immutable deferred DMA publications, invalid timer updates,
fractional prescalers, power interruption, common GPIO/PWM wiring, Z gearing,
exception cleanup and OpenGL context ownership. Run `simulation.regressions`
and `simulation.machine` under the sanitizer preset for native model checks.
