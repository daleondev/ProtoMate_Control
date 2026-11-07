# Firmware profiling and tracing

The two GIGA cores record independently. ThreadX events and application events
share each core's native TraceX ring. There is no logging/formatting, allocation,
USB traffic, or ICC request in an event insertion. Capture export uses Atmel-ICE
SWD after recording stops. The logic analyzer is a separate electrical test tool.

## Builds and memory

```sh
cmake --preset giga-profile
cmake --build --preset giga-profile
python3 scripts/giga_program.py flash --build build/giga-profile
```

`giga-io-profile` and `giga-ui-profile` inherit the ordinary Release optimization,
clock, runtime, and application configuration. They add `PROTOMATE_PROFILING` and
ThreadX event trace support consistently to the runtime and its consumers.
`profile-linux` exercises the same recorder with the hosted ThreadX runtime.
It uses elapsed microseconds instead of DWT cycles. Linux UI/IO share one recorder;
the native simulator coordinator does not contain another ThreadX recorder.

| Core | Native trace ring | Summary reservation | Current summary |
|---|---|---|---|
| M4 | `0xC0400000`, 128 KiB | `0xC0420000`, 32 KiB | 8,232 bytes |
| M7 | `0xC0440000`, 128 KiB | `0xC0460000`, 32 KiB | 8,232 bytes |

These addresses are in the GIGA's **external AS4C4M16SA SDRAM**, not M4 SRAM.
The existing MPU maps this region as normal, non-cacheable, shareable and
execute-never. There is one writer per buffer; the other core does not use it
as a queue. Linker regions/assertions reserve it explicitly. Profiling cannot
be combined with M4 SDRAM-code, motion-benchmark, or other overlapping presets.

Small recorder state, a first-failure record, allocator recursion guards, and
per-thread cycle counters remain in internal RAM. IRQ/scheduler hooks update
internal counters; the existing diagnostic worker copies totals to SDRAM once
per second and on ordinary freeze. No profiling worker or additional heap-backed
buffer is created. Fatal/stack failure paths only update internal state and
stop trace insertion; their CPU summary can be up to one sample interval old.

The profiling M4 image currently uses 196,568 of 196,608 bytes of instruction
RAM (40 bytes remain). Adding more hot instrumentation requires another linked
memory/call-path review. Ordinary Release uses 193,264 bytes in that region.

M4 insertion, accounting, IRQ and fault paths remain in instruction RAM and are
included in the linked-image RAM audit. Recorder setup, summary aggregation,
registry maintenance and allocator wrappers execute from QSPI. No original
motion call path was moved out of instruction RAM to make profiling fit.

Normal builds retain allocation-failure/heap health and the internal failure
record. They do not reserve these SDRAM areas or compile detailed events/scopes.

## Capture workflow

Disable motors before arming an M4 capture. Arming clears the fixed ring and
registers current ThreadX objects; it is a setup operation with interrupts
masked. It does **not** disable motors for you. Recording can subsequently run
while application work is exercised.

USB CLI:

```text
profile status
profile arm system
profile freeze
profile summary
```

Use `ui`, `io`, or `all` (default) after the operation/preset. `summary` prints
compact health and capture counters; the exported report contains full duration,
CPU, stack and queue summaries. On Linux, `all` operates on the shared recorder
once.

| Preset | Application events | Native kernel events |
|---|---|---|
| `system` | capture, health, scheduling, ICC frames/stages | Full except timer/time calls |
| `motion` | system + motion scopes/stages + external IRQ boundaries | Scheduling/thread/queue; bulk mutex/semaphore/event-flag/pool calls filtered |
| `graphics` | system + rendering/presentation + external IRQ boundaries | Same reduced kernel set as motion |
| `memory` | system + allocation operations | Full except timer/time calls |

SysTick CPU time is accounted, but its 1 kHz entry/exit events are omitted.
These are flight recorders: after wrapping they retain the newest events.
Full kernel recording can fill a ring quickly; always inspect the actual
retained interval, wraps and registry capacity instead of assuming a fixed
number of seconds. Summaries cover the entire capture even after ring overwrite.

SWD mailbox control also works when ICC is unavailable:

```sh
python3 scripts/profile.py arm --preset motion
# Exercise application work through the normal CLI/GUI.
python3 scripts/profile.py freeze
python3 scripts/profile.py export --output build/profiles/my-capture
```

The tool checks the active image-pair manifest against **both supplied ELFs
before writing a mailbox**. It attaches without asserting reset, runs no target
CRC/flash algorithm, and exports only frozen, stable data. Do not have another
OpenOCD instance open. Use `--m4` and `--m7` for non-default image paths.
`status` also supports ordinary Release ELFs to retrieve their internal health
record. A reset/reflash destroys a capture; export before doing either.

The debugger writes request arguments and a sequence number. Existing UI logging
and IO diagnostic workers acknowledge completion. A dead worker cannot acknowledge
a new request: the tool times out rather than claiming capture succeeded.
Fatal handlers can already have frozen the recorder, allowing direct export.

## Triggers and event meaning

A recording freezes locally on an allocation failure, terminal ICC failure,
STEP DMA/underrun fault, executor schedule failure, stack fault, or fatal error.
Output inhibition precedes STEP fault recording. Recording does not implement
or replace motion safety, the producer lease, or the hardware guard.

The first failure contains the reason, operation, requested byte count (or
operation argument), caller PC, current thread address and TIM5 timestamp.
A subsequent exception cannot overwrite that trigger. Allocator interception
preserves Newlib malloc/calloc/realloc/memalign return values and ownership;
zero-size null results are not classified as allocation failures. Nested
allocator calls are counted once using bounded per-thread guards.
For a stack-check failure, the `caller` field identifies the offending ThreadX
thread control block rather than an instruction address. The record's `thread`
field identifies the thread running the check; they need not be the same.

Motion failures distinguish executor schedule codes (`operation & 0x10000`,
argument = axis) from STEP engine codes (`operation & 0x20000`, argument = source
line). Resolve them against the captured ELF/source revision.

Stable user event IDs live in `platform/profiling/Trace.h`:

- 4096–4104: capture, shared-time anchors, failure, allocation, duration, queue,
  heap, reserved stack event, actual scheduled-thread change.
- 4120–4122: raw ICC stages, connection state, frame kind/lane/correlation ID.
- 4140–4141: motion stages/jobs.
- 4160: graphics/diagnostic markers.

Instrument thread work with `profiling::Scope{site, category}`. Scopes started
outside recording or spanning a rearm do not enter the new capture. Elapsed DWT
cycles **include preemption**; they are not exclusive function CPU time.
Scopes spanning a complete DWT revolution are counted as invalid. DWT wraps in
about 17.9 s on M4 and 8.95 s on M7. Extended CPU totals use frequent runtime/IRQ
samples, so they can cover longer sessions.

All event timestamps use the **same running M4 TIM5**, at 1 MHz with a 32-bit
modulus (about 71.6 minutes). Local DWT counters are never directly compared
between cores. Once-per-second anchors record timestamp wraps. Captures are
identified by a common ID and exact image pair; mismatched captures are rejected.

The browser report correlates nonzero frame IDs from send submission to peer
dispatch. That interval includes queueing, wakeup and decoding. It is not an
RTT/2 estimate. Topic IDs are zero, so they are intentionally not correlated.
Unmatched endpoints after ring overwrite are reported, not interpreted as lost
application messages. Use a new capture after any restart; never merge reset
sessions just because a timer value looks similar.

Queue slots 0–2 are TX normal/urgent/background; 3–5 are RX lanes. These contain
capture-local contention counts. Slot 6 is UI's log subscription; slots 16–31
are outgoing topics in registration order. Subscription drops/replacements are
sampled **lifetime totals**, with single-entry subscriptions labelled as latest
value replacement. Their high-water mark is sampled, not an asserted absolute
peak. Hardware-ring peaks are observed at send/receive operations.

Heap minimum-free and stack high-water data are sampled. Heap values are not
largest-free-block guarantees. Thread cycles are per address, not per priority;
the bounded table reports overflow rather than silently assigning time to another
thread. Address reuse within a capture combines that address's lifetime. The idle
bucket includes scheduler transition work. DWT coverage and accounted running
share are reported separately; neither is presented as a proven worst-case CPU
bound. Fatal captures may have incomplete final summary sampling.

## Files and viewers

An export contains:

- `m4.trx`, `m7.trx`: unmodified native TraceX buffers.
- Control and summary dumps; exact `m4.elf`/`m7.elf` copies.
- `manifest.bin`, firmware/source hashes, source diff, changed-source archive
  (including new files), and dependency revision.
- `report.json`: durations/histograms, CPU accounting, heap/stack/queue data,
  overwrite/resource limits and cross-core latency statistics.
- `timeline.json`: import into **https://ui.perfetto.dev/** for a combined timeline.

Duration histograms have power-of-two microsecond buckets; the last bucket is
open-ended. Do not treat a bucket edge as an exact percentile. Native ThreadX
resume events mean a thread became runnable; event 4104 records the actual
execution hook. Application duration bars can overlap those thread events.

The official TraceX source is available under `external/tracex`. Its viewer is
Windows/.NET WPF. Wine is not installed on this workstation, so viewer operation
under Wine is **not verified**. The native trace format and browser exporter are
available independently; no custom live-streaming collector is required.

## Controlled checks

Only on a diagnostic bench with motors disconnected:

```sh
python3 scripts/profile.py arm --preset system
python3 scripts/profile.py calibrate
python3 scripts/profile.py freeze
python3 scripts/profile.py export --output build/profiles/calibration

python3 scripts/profile.py arm --preset memory
python3 scripts/profile.py allocation-failure --core 4
python3 scripts/profile.py freeze
python3 scripts/profile.py export --output build/profiles/allocation-failure
```

Calibration uses 256 bounded samples: site 62 measures the empty DWT pair and
site 63 measures one user event. IRQs are masked for each individual sample,
not the entire batch. Compare distributions and observed maxima. These do not
measure every registry/object-creation cost or constitute a worst-case guarantee.
The failure check requests an impossible allocation without changing motor state.

```sh
cmake --preset profile-linux -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$PWD/build/debug-linux/_deps/googletest-src"
cmake --build --preset profile-linux
ctest --test-dir build/profile-linux --output-on-failure
python3 -m pytest scripts/tests/test_profiling.py
```

The decoder tests also run as CTest's `tooling.profiling`. To repeat the
motor-disabled application stability check on the profiling image:

```sh
python3 scripts/profiling/verify_hardware.py --seconds 600 --output build/profiles/health-check
```

This checks GUI/ICC/CLI health, switches diagnostic display modes, compares
command round trips with recording frozen/active, and exports the result. It
does not initialize or enable drivers, generate motion, or use the analyzer.
It requires zero existing display errors and a fresh output directory. A
failed SWD operation preserves its OpenOCD log in `build/profiles/errors`.

After testing, restore ordinary firmware with `giga-release` and
`python3 scripts/giga_program.py flash --build build/giga-release`.
