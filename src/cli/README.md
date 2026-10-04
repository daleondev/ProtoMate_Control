# CLI parser

The CLI accepts shell-like whitespace, single and double quotes, backslash
escaping, empty quoted arguments, and a built-in `help [command]` command.
Command paths can contain multiple words, such as `motor move`; the longest
registered path wins. Use `help motor move` for that command's flags and units.
Arguments are positional. Required arguments must be declared before optional
arguments so parsing remains unambiguous. A final argument may be marked
`variadic` to accept any number of remaining values.

Registered flags support short and long forms, bundled boolean flags such as
`-al`, value forms such as `-o file`, `-ofile`, `--output file` and
`--output=file`, and `--` to stop flag parsing.

Negative numeric arguments (`-90`, `-.5`, `-1e-3`) work without `--`, unless
their first digit is explicitly registered as a short flag.

Unquoted, unescaped `*` characters expand against the CLI-local working
directory before argument parsing:

```text
ls *.txt
cat logs/*.txt | grep error
cp assets/*.bin backup
rm build/*.tmp
```

`*` matches within one path component and does not cross `/`. Results are
sorted, hidden names only match patterns whose component starts with `.`, and
an unmatched pattern is passed to the command unchanged. Use `"*.txt"` or
`\*.txt` when a literal asterisk is required. Expansion is capped at 256
matches to keep embedded memory use bounded.

Pipelines and output redirection are supported, including operators adjacent
to words:

```text
cat messages.txt | grep -in warning
echo "first line" > notes.txt
echo "another line">>notes.txt
```

`>` truncates and `>>` appends. The destination is resolved against the
CLI-local working directory. Operators inside quotes, or escaped with a
backslash, remain ordinary argument text. Redirection must follow the final
pipeline command. Intermediate pipeline output is bounded to 32 KiB so a
command cannot exhaust STM32 memory.

Commands are annotated functions in their module's `commands` namespace,
registered through `register_commands()` before `system_threads::start()`
launches the CLI thread. The first annotation is the description; `Arg` and
`Flag` annotations supply parser validation and help text. The function name
is used by default. An optional `Name` annotation supplies a multiword command
path or another explicit name.

```cpp
#include "cli/Parser.hpp"
#include <ostream>

namespace cli::example
{
    namespace commands
    {
        using namespace pnm::meta::string::literals;

        [[
            = "Show a source and optional destination."_fs,
            = Name{ "example copy"_fs },
            = Arg{ .name = "source"_fs, .description = "Source path"_fs },
            = Arg{ .name = "destination"_fs, .description = "Destination path"_fs,
                   .optional = true },
            = Flag{ .name = "force"_fs, .short_name = 'f',
                    .description = "Replace destination"_fs },
            = Flag{ .name = "mode"_fs, .short_name = 'm',
                    .description = "Copy mode"_fs, .value_name = "name"_fs }
        ]] static auto copy(const Arguments& args, std::ostream& out) -> CallbackResult
        {
            out << args.require("source") << '\n';
            if (const auto destination{ args.get("destination") })
                out << *destination << '\n';
            return 0;
        }
    }

    void setup() { register_commands(); }
}
```

`register_commands(parser, bind)` registers into a specific parser and applies
`bind` to each reflected function to obtain its callback. Motor, axis and robot
modules use this to capture their application-owned controller/robot, then pass
it by reference to the annotated handler. Each parser retains its own bindings;
there is no global controller. The stateless filesystem and utility modules
use the default identity binding and application registry.

The command modules are `motor.cpp`, `axis.cpp` and `robot.cpp`. Shared parsing
and result formatting live in `control_common.cpp`; command descriptions,
arguments and flags live beside their handlers. `Parser::registerCommand()`
remains the underlying runtime API used by the reflection registrar.

Callbacks returning `int` remain supported, and that value is exposed as
`ExecutionResult::exit_code`. For an operational failure, return
`cli::callback_failure("diagnostic", exit_code)`. The parser turns it into a
`CallbackFailed` result, prefixes it with the command name, and displays it in
both terminal frontends. Callback exceptions are still contained as a final
safety boundary and cannot terminate the CLI loop.

Commands that consume pipeline input use the stream-aware callback form:

```cpp
.callback =
  [](const cli::Arguments&, cli::CommandIO& io) {
      std::string line;
      while (std::getline(io.input, line)) {
          io.output << line << '\n';
      }
      return 0;
  },
```

The existing `(const cli::Arguments&, std::ostream&)` form is automatically
adapted and remains source-compatible.

The built-in filesystem command set contains `pwd`, `cd`, `ls`, `touch`,
`mkdir`, `rmdir`, `rm`, `cp`, `mv`, and `cat`. Paths are resolved against a
CLI-local working directory, so using `cd` does not alter the process working
directory or other application threads. Utility commands include variadic
`echo` (`-n`) and fixed-text `grep` (`-i`, `-n`, and `-v`). `grep` reads its
optional file argument or consumes standard input from the previous pipeline
stage. `stack` prints per-thread stack usage, and `heap` prints allocator
capacity, reservation, usage, available space, and fragmentation details with
a vertical usage bar spanning the metric rows.
Linux obtains heap statistics from glibc and reports its capacity as dynamic;
STM32 combines Newlib allocator statistics with the linker-defined AXI-SRAM
heap bounds.

## Motor control

The normal application image (`debug-stm32` or `release-stm32`) exposes the
commands below on the UART CLI. Linux exposes the same commands with simulated
peripherals. Boot starts the shared timebase with all drivers disabled; it
submits no motion. `motor` prints the overview. `help motor <command>` describes
the arguments and flags. Motor selectors are `m1`, `m2`, `m3` or `1`, `2`, `3`.

All positions are **motor-shaft degrees** and speeds are positive **rpm
magnitudes**, including M3. The sign of a relative displacement, or the
absolute target relative to the current position, determines direction.
Joint gearing and Z millimetres are available through the `axis` commands
below. Tool coordinates and kinematics calculations are available through
the `robot` commands.

| Command | Behavior |
| --- | --- |
| `motor status [motor\|all]` | Live commanded position/speed, encoder feedback, reference/switch state, outstanding command count and generator state. Defaults to all. |
| `motor enable` | Enable all three drivers and wait 200 ms for settling. |
| `motor disable` | Abort all active/queued motions, remove holding torque and invalidate all references. |
| `motor home <motor>` | Reference one motor: seek, back off, slowly re-latch and set its configured reference position. |
| `motor move <motor> <degrees> --speed <rpm>` | Relative move; negative angles move away from the reference switch. |
| `motor moveto <motor> <degrees> --speed <rpm>` | Absolute move; requires successful referencing. |
| `motor speed <motor> <rpm>` | Replan the active move's uncommitted tail, preserving endpoint and dynamics limits. |
| `motor stop [motor\|all]` | Immediately abort active and queued motions, without a deceleration ramp. Holding torque stays enabled. Defaults to all. |
| `motor defaults <motor>` | Show dynamics; optionally change `--accel`, `--decel`, `--jerk` while that motor is idle. Settings are held in RAM. |
| `motor jobs [id]` | Show the latest 32 commands or one result; outstanding commands are retained. |
| `motor reset` | Explicitly restart a stopped/faulted timebase. Requires disabled drivers and leaves them disabled/unreferenced. |

For example, enter these interactively, inspecting each motion's result before
issuing a dependent command:

```text
motor status
motor enable
motor move m1 -45 --speed 5
motor jobs 1
motor status m1
motor home m1 --seek 5 --latch 0.5 --timeout 30
motor jobs 2
```

After job 2 completes successfully, an absolute move below the configured
135-degree reference coordinate moves away from the switch:

```text
motor moveto m1 90 --speed 10 --accel 720 --decel 720 --jerk 7200
motor jobs
motor stop m1
motor disable
```

Moves/home return a motion ID without waiting for completion. `outstanding`
includes planning, execution and queueing; it does not mean the command has
completed successfully. Terminal results are `completed`, `stopped`,
`timed-out`, `rejected` or `faulted`. The prompt remains available for status and
stop commands. `motor jobs <id>` returns a nonzero exit code for a terminal
failure or an unknown/expired ID. Submission also returns nonzero when the
motor rejects it synchronously. No extra thread polls the motors for the CLI;
result queries inspect futures without waiting, and motor accounting remains
callback-driven.

Both move commands accept these optional flags:

| Flag | Units / meaning |
| --- | --- |
| `--accel` | Nonnegative degrees/s²; 0 selects the axis default. |
| `--decel` | Nonnegative degrees/s²; 0 selects the axis default. |
| `--jerk` | Nonnegative degrees/s³; 0 selects the axis default. |
| `--timeout` | Seconds after command activation; 0 means unlimited. |
| `--buffer` | `aborting` (default), `buffered`, `blending-low`, `blending-previous`, `blending-next`, `blending-high`. |

`aborting` replaces this motor's current/queued work. `buffered` waits for a
full stop. Blending requests a continuous same-direction transition when
feasible; a late or infeasible blend falls back to buffered execution. Each
motor supports one pending successor. The modes do not synchronize different
axes. For example, while a sufficiently long M2 move is running:

```text
motor move m2 -720 --speed 10
motor move m2 -90 --speed 20 --buffer blending-low
motor jobs
```

`motor speed` requires an active, started move with uncommitted pulses, and
can reject an update that cannot fit. It reports the first affected pulse
number. `motor home` defaults to 5 rpm seek, 0.5 rpm latch and a 30-second
overall timeout. Latch speed must be strictly slower than seek speed. Homing
aborts existing commands on that motor. An active/open reference input blocks
approach and permits retreat; `active/open` cannot distinguish physical
activation from a disconnected NC switch.

Axis dynamics initially use 3600 degrees/s² acceleration/deceleration and
36000 degrees/s³ jerk. `motor defaults m1 --jerk 0` selects trapezoidal motion;
in a **move** command, `--jerk 0` instead selects the configured default.

## Joint-axis control

`axis ...` uses the existing `RotaryAxisConversion` and `LinearAxisConversion`
layer. It commands the **same motors** as `motor ...`, with the same enable,
reference state, active/queued motions, defaults and motion IDs. There is no
separate axis timebase or polling thread. `axis` prints an overview and
`help axis <command>` describes its options.

| Axis | Motor | Selectors | Position | Speed | Acceleration/deceleration | Jerk |
| --- | --- | --- | --- | --- | --- | --- |
| Shoulder | M1 | `shoulder`, `a1`, `1` | degrees | degrees/s | degrees/s² | degrees/s³ |
| Elbow, relative to first arm | M2 | `elbow`, `a2`, `2` | degrees | degrees/s | degrees/s² | degrees/s³ |
| Vertical | M3 | `z`, `a3`, `3` | mm | mm/s | mm/s² | mm/s³ |

`main.cpp` supplies editable **dummy mechanics** through
`MotionController::configureAxis()` before the CLI starts:

| Setting | Shoulder | Elbow | Z |
| --- | --- | --- | --- |
| Motor/output reduction | 1:1 | 1:1 | 1:1 |
| Direction | Same as motor | Same as motor | Same as motor |
| Joint coordinate at reference switch | 0 degrees | 0 degrees | 0 mm |
| Travel per output revolution | — | — | 40 mm |

These are placeholders, not measured mechanical values. Replace the values in
`main.cpp` when the actual transmission and coordinate conventions are known.
The motor-side reference is taken from the existing motor configuration, so
both layers describe the same physical switch position. There is no interactive
mechanics configuration command. Startup still leaves all drivers disabled and
all motors unreferenced.

The motion commands mirror the motor commands:

```text
axis status
axis enable
axis move shoulder -5 --speed 10 --accel 100 --decel 100 --jerk 1000
axis jobs
axis move z -2 --speed 1
axis jobs
axis stop shoulder
axis disable
```

Use `axis moveto <axis> <position> --speed <value>` for absolute moves after
successful referencing. `axis home <axis>` runs the same seek/release/latch
sequence, assigning the configured joint home coordinate through the converter.
Optional `--seek` and `--latch` are joint-speed magnitudes; omitted values retain
the existing 5 rpm and 0.5 rpm **motor** speeds. With the dummy mechanics these
are 30/3 degrees/s for shoulder/elbow and approximately 3.333/0.333 mm/s for Z.
The overall `--timeout` defaults to 30 seconds.
The seek direction remains motor Forward, which may be negative in joint
coordinates when `AxisDirection::OppositeToMotor` is configured in code.

`axis speed <axis> <value>` replans the active motion's remaining speed using
joint units. `axis defaults <axis> [--accel ...] [--decel ...] [--jerk ...]`
reads/writes the same motor profile limits through the converter. Zero move
overrides select those defaults; a configured default jerk of zero selects a
trapezoid. Move `--buffer` modes and `--timeout` have the same semantics as
their motor-command equivalents. Relative moves use displacement conversion
without reference offsets; absolute moves use position conversion with offsets.
Speed and profile-limit magnitudes remain nonnegative under direction inversion.

`axis status [axis|all]` displays signed joint velocities and converted encoder
feedback, preserving missing/faulted encoder results. Before referencing, the
displayed positions do not establish a physical datum. M2/M3 have no encoder
feedback. `axis jobs [id]` shows the same result list as `motor jobs`, using
joint names. `axis stop [axis|all]` works even before configuring mechanics.
`axis enable`, `axis disable` and `axis reset` act on the shared driver enable
and generator, exactly like the motor equivalents.

## Robot kinematics

The `robot` commands use millimetres for tool XYZ/Z travel and degrees for
shoulder/elbow angles. Tool yaw is derived from the two joints, not independently
commandable. Run `robot` for an overview or `help robot ik` for argument details.

| Command | Behavior |
| --- | --- |
| `robot status` | Live commanded joint/tool position and tool velocity; reference, generator and encoder validity. |
| `robot geometry` | Arm lengths, base frame, tool offset/yaw, joint limits and singularity threshold. |
| `robot fk <shoulder> <elbow> <z>` | Forward calculation from joint coordinates to tool XYZ and yaw. |
| `robot ik <x> <y> <z> [--branch current\|positive\|negative]` | Inverse calculation using current commanded joints as the seed; outputs a joint endpoint. |

```text
robot status
robot geometry
robot fk 30 60 50
robot ik 300 200 40 --branch positive
```

FK and IK are calculations and start no motion. IK preserves the current elbow
branch by default, selects equivalent angles nearest the unwrapped seed, and
checks configured joint limits. A singular seed requires an explicit branch.
Unreachable targets, singular solutions and limit violations report errors.
Selecting an endpoint does not validate a path or synchronize axis motions.

`robot status` converts emitted-step coordinates and signed speeds through the
axis conversions and SCARA geometry. Unreferenced coordinates lack a physical
datum; uncertain pulse counts remain marked as uncertain. A fully measured tool
state requires valid encoders on all three axes. With feedback only on M1 it is
reported as unavailable; use `axis status` for individual encoder readings.
Motor readings are sampled sequentially, not latched at one hardware instant.

`main.cpp` supplies nominal CAD arm lengths of 205 and 223.4 mm and a 135 mm
forward tool offset. Base translation/yaw and the remaining tool offsets/yaw are
zero, with no configured joint limits. Axis mechanics remain explicit dummy
values in code. Replace these configuration values with calibrated mechanics
and frames before relying on physical tool coordinates.

### Ownership

`main()` is the composition point. It creates one
`control::MotionController` with the three axis configurations, then passes
its shared pointer to `cli::motor::setup(parser, controller)`,
`cli::axis::setup(parser, controller)` and `control::Robot`. It registers
`cli::robot::setup(parser, robot)` before starting application threads.
The controller owns the generator, shared enable, axis conversions and
three motors; it starts the timebase once, serializes client operations and
tracks command results. CLI callbacks retain that controller and never create
peripherals. Other application code can submit and inspect the same motions
through this API without depending on the CLI.
Submission returns a handle containing a `shared_future` as well as the ID,
so another client can await completion without polling `motions()`. A retained
handle remains valid after its entry expires from the 32-command result list.

Axis moves, home speeds, live speed updates and profile defaults are converted
inside the controller under its submission lock. A calibration update cannot
interleave between conversion and submission. Status includes the conversions
from the same locked snapshot as the motor readings. The strong `AxisMove`,
`AxisSpeed` and `AxisDefaults` variants reject rotary/linear unit mismatches.

`Robot` owns the configured `ScaraKinematics` instance and retains the same
controller. It computes state on demand without a second set of motors, another
worker or a global singleton. The geometry class stays independent of HAL and
motion execution. A future coordinated planner must arbitrate manual-axis
commands against robot motions; per-call serialization alone is not
coordinated-motion ownership or synchronized multi-axis execution. Cartesian
motion commands are not registered until those operations exist.

## Linux terminal frontend

When both stdin and stdout are attached to a supported terminal, the Linux
simulation uses an ncursesw frontend. HAL and Pneumo log records remain in the
scrollable upper area while command input stays on the bottom line. Use the
arrow keys for editing and history, Page Up/Page Down for log history, and
Ctrl-C to close the CLI frontend.

Redirected input/output, `TERM=dumb`, and
`SIMPLCITY_CLI_PLAIN=1` select the plain stream frontend instead. This keeps
the CLI scriptable: commands and prompts use stdout, while HAL and logging
records use stderr. The STM32 build continues to use the original
`std::cin`/`std::cout` serial frontend.
