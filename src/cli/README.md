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

Register commands before `system_threads::start()` launches the CLI thread:

```cpp
#include "cli/Parser.hpp"

auto result = cli::registry().registerCommand({
  .name = "copy",
  .description = "Copy a source to an optional destination",
  .arguments =
    {
      { .name = "source", .description = "Source path" },
      { .name = "destination", .description = "Destination path", .optional = true },
    },
  .flags =
    {
      {
        .name = "force",
        .short_name = 'f',
        .description = "Replace an existing destination",
        .value_name = std::nullopt,
      },
      {
        .name = "mode",
        .short_name = 'm',
        .description = "Select the copy mode",
        .value_name = "name",
      },
    },
  .callback =
    [](const cli::Arguments& arguments, std::ostream& output) {
        output << "source: " << arguments.require("source") << '\n';
        if (const auto destination = arguments.get("destination")) {
            output << "destination: " << *destination << '\n';
        }
        output << "force: " << arguments.hasFlag("force") << '\n';
        if (const auto mode = arguments.flagValue("mode")) {
            output << "mode: " << *mode << '\n';
        }
        return 0;
    },
});

if (!result) {
    throw std::runtime_error(result.error());
}
```

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
Joint gearing, Z millimetres and Cartesian coordinates are not applied by
these commands.

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

### Ownership and future robot commands

`main()` is the composition point. It creates one
`control::MotionController` with the three axis configurations, then passes
its shared pointer to `cli::motion::setup(parser, controller)` before starting
application threads. The controller owns the generator, shared enable and
three motors; it starts the timebase once, serializes client operations and
tracks command results. CLI callbacks retain that controller and never create
peripherals. Other application code can submit and inspect the same motions
through this API without depending on the CLI.
Submission returns a handle containing a `shared_future` as well as the ID,
so another client can await completion without polling `motions()`. A retained
handle remains valid after its entry expires from the 32-command result list.

Construct the future `Robot` at that same composition point and inject the
same controller. Robot-level axis conversion, kinematics and coordinated
planning belong above this boundary. A separate command module can register
`robot ...` paths with callbacks capturing that Robot, alongside these
`motor ...` commands. No second set of motors or global Robot singleton is
needed. A future coordinated planner must arbitrate manual-axis commands
against robot motions; per-call serialization alone is not coordinated-motion
ownership or synchronized multi-axis execution. Cartesian command names are
not registered until those operations exist.

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
