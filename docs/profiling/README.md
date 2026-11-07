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
The SWD control/failure mailbox and runtime CPU-sampling counter use uncached
DTCM on M7 and uncached D2 SRAM on M4. The export tools reject symbols outside
those regions; `volatile` alone would not make cacheable M7 RAM coherent with SWD.

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
The kernel event-name catalog is copied from the pinned ThreadX header into
each export. `profile.py decode --output <capture>` regenerates reports under
`<capture>/decoded/`, preserving the original files and their hashes.

The official TraceX source is available under `external/tracex`. Its viewer is
Windows/.NET WPF. Wine is installed, but no TraceX viewer binary or dedicated
viewer prefix is currently installed; a trusted official download is pending.
The native trace format and Python/Perfetto exporter remain available without
it. A future viewer should use **1 tick per microsecond** for our 1 MHz TIM5
clock. Each native trace covers one core; the Perfetto export combines both.

A separate Wine prefix separates application configuration, not host access.
It is not a security sandbox. Verify the provenance of a replacement viewer
before running it; no custom live-streaming collector is required.

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
not the entire batch. Compare cycle minima, means and observed maxima. These
samples do not collect a microsecond histogram inside the probe. They do not
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
command round trips with recording frozen/active, and exports the result. Those
round trips include the host serial read timeout; they are not CPU timings. It
does not initialize or enable drivers, generate motion, or use the analyzer.
It requires zero existing display errors and a fresh output directory. A
failed SWD operation preserves its OpenOCD log in `build/profiles/errors`.

For an overhead comparison, sample the existing runtime counters with tracing
frozen and active, using the same workload:

```sh
python3 scripts/profiling/sample_cpu.py --output build/profiles/cpu-frozen
python3 scripts/profile.py arm --preset motion
python3 scripts/profiling/sample_cpu.py --output build/profiles/cpu-recording
python3 scripts/profile.py freeze
```

`sample_cpu.py` checks the image pair and uncached symbol locations and never
halts or resets the cores. Its thread-running fraction is not an exclusive
CPU-utilization measurement; boundary samples are asynchronous. Supply the
ordinary Release ELFs with `--m4` and `--m7` to compare against normal firmware.

After testing, restore ordinary firmware with `giga-release` and
`python3 scripts/giga_program.py flash --build build/giga-release`.

## Verification on 2026-11-07

All 81 profiling-host and 80 normal-host CTest suites passed, including the
six recorder cases and 16 Python decoder cases. GIGA profiling, ordinary
Release and ordinary Debug builds passed. The M4 link audit found no required
interrupt/motion call path outside instruction RAM.

A 601-second GUI/CLI/ICC session completed with no allocation failures, link
loss, LTDC underruns or DMA errors. Sampled minimum heap availability was
44,328 bytes on M4 and 39,056 bytes on M7; minimum thread-stack headroom in
that session was 42% and 38%. A further 31-second final-image check verified
the uncached SWD mailbox and freeze/export behavior. The evidence records each
image pair separately. Both cores also captured the deliberate impossible
allocation, retained its first-cause record and continued running.

The final 256-sample calibration measured one user-event insertion at:

| Core | Mean | Observed maximum |
|---|---:|---:|
| M4 | 7.73 µs | 57.68 µs |
| M7 | 0.41 µs | 6.53 µs |

These include SDRAM/flash contention during the sample; they are not maximum
execution-time guarantees. Ten-second samples of the existing runtime
thread-running counters, with the ordinary Home screen active, gave:

| Build / recording state | M4 | M7 |
|---|---:|---:|
| Ordinary Release | 80.87% | 50.11% |
| Profiling image, frozen | 88.13% | 57.21% |
| Profiling image, `motion` recording | 94.21% | 56.92% |

The small M7 difference between frozen and recording is within variation
between these short runs; it does not demonstrate a speedup. Even a frozen
profiling build has instrumentation/kernel overhead. Use normal firmware as
the baseline and do not transfer instrumented timing results directly to it.
The retained rings contain only a small recent window at these event rates;
there were too few paired ICC endpoints in the exports to claim a latency
percentile distribution.

[Verification record](verification.json) contains image IDs, memory and timing
measurements, test counts and local artifact locations. Exact native captures,
ELFs, source archives, manifests, hashes and host-test logs remain under
`build/profiles/`. Normal Release firmware was restored and both cores were
verified Ready, connected and fault-free, with motors disabled. Powered motion
and physical display behavior are not claimed as verified by this profiling
campaign. The official Windows viewer remains to be obtained and verified.
