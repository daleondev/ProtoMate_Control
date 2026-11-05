# GIGA motion-architecture measurement image

This paired image is deliberately separate from the application. It runs the actual
ThreadX runtime, ICC channel/transport/bridge, GUI and STEP backend, with motor
enable held **HIGH (inactive)**. It does not expose experimental trajectories through
the production motion service. See [measured gates and limitations](../../docs/motion-architecture/README.md).

The initial production-executor target is **3 × 12.5 kHz**, with unchanged 5 ms
admission and 20 ms adjustment-onset targets. The 100 kHz constant-output and
kernel fixtures remain stress/characterization cases. Passing a finite profile
does not qualify stream-loss stopping or live retiming.

## Build, flash and restore

```sh
cmake --preset giga-motion-bench-release
cmake --build --preset giga-motion-bench-release -j4
python3 scripts/giga_program.py flash --build build/giga-motion-bench-release
python3 examples/giga_motion_bench/run.py --output build/my-campaign/clock.jsonl clock
# Debug correctness uses giga-motion-bench-debug and run.py --build build/giga-motion-bench-debug.

# Restore the normal paired application after testing:
cmake --build --preset giga-debug -j4
python3 scripts/giga_program.py flash --build build/giga-debug
```

The runner verifies the running image-pair identifier, archives firmware by SHA-256,
and refuses to overwrite evidence. Close other serial clients first. Debugger PC
sampling is a separate characterization experiment, not a qualification capture.

Keep motor power disconnected. Analyzer wiring, at 24 MHz:

| Analyzer | GIGA | Signal |
|---|---|---|
| D0 | A7 | M1 STEP |
| D1 | D3 | M2 STEP |
| D2 | D2 | M3 STEP |
| D3 | D15 | Combined enable, HIGH inactive |
| D4 | D14 | M7 command-window marker |
| D5 | D17 | M4 command-window marker |
| D6 | D20 | Synthetic CPU-load window |
| D7 | D21 | STEP progress callback marker |
| GND | GND | Common reference |

Marker pins must otherwise be disconnected. D20 is **not** driven into a reference
switch by these tests. D4/D5 delimit entire benchmark commands, not each individual
admission. D7 is progress notification, not proof of the first changed STEP edge.

## Console commands

Use `run.py --output … 'command' …`; commands finish with `[motion-bench] DONE`.

| Command | Meaning |
|---|---|
| `identity` | Running pair hash and boot token |
| `clock` | TIM5 / DWT frequency relationship and instrumentation overhead |
| `trace on` / `trace off` | Bounded, correlated internal event recording |
| `priority 5` | Change only the M7 command worker; ordinary default is 15 |
| `raw bytes rate count direction lane` | Raw SPSC queue measurement |
| `isolation count` | Fill normal/background queues, exchange urgent frames, then verify queued data |
| `transport bytes rate count direction lane` | Coroutine transfer measurement |
| `bridge bytes rate count 1 lane` | Sequential UI→IO service calls; includes intended-submission backlog |
| `topics bytes rate count` | UI→IO latest-only topic stream; coalescing counted explicitly |
| `actions count 0` / `actions count 1` | Accepted action, feedback and success / requested cancellation and terminal acknowledgement |
| `flood count` | Urgent calls with a two-second priority-1 burst overload on normal/latest and bounded background topics |
| `kernel fixture count [priority]` | Identical algorithm fixtures on each core; dedicated 20 KiB M4 / 24 KiB M7 worker; M4 stack uses AXI heap, M7 static storage |
| `dispatch mode` | Select ordinary dispatch (0), priority experiment (1), or bounded coroutine polling experiment (2), before constructing ICC |
| `fast rate count fixture` | Native bounded command path; fixture 0 alone, 1 ordinary ICC flood, 2 additionally 3 × 100 kHz STEP and fixed-work M4 load |
| `stopping rate count method` | Full three-axis stopping timestamp cost; method 0 reference inversion, 1 bounded fixed-point evaluator |
| `profile rate cycles load [shape] [half]` | Finite jerk-limited profiles, ≤25 kHz; shape 0 scaled / 1 equal peaks; half 64/128/256 pulses, default 64 |
| `executor cycles injection_ms mode load [shape] [traffic]` | Production finite executor, 12.5 kHz peak and 32-pulse halves; mode 0 completes / 1 cancels / 2 stops keepalives / 3 stalls its worker for 700 ms; load 0/20; shape 0 equal / 1 scaled peaks / 2 unequal durations; traffic 0 normal / 1 flood |
| `pulses rate count load` | Three simultaneous constant-rate outputs, GUI + service traffic, load setting 0/10/20/40 |
| `status` | GUI presentation / underrun / error counters |
| `reset` | Coordinated firmware restart; serial disconnects |

Directions: 0=M4→M7, 1=M7→M4, 2=both. Lanes: 0=normal, 1=urgent,
2=background. Background frames are ≤512 bytes; other raw frames ≤2048.
Bridge/topic strings are conservatively limited to 2000 bytes (name, length and
header consume the remaining space). Services cannot use background.

Within a boot, run **raw/isolation → transport → bridge/topics/actions/pulses**.
Reset to return to an earlier layer. A bridge permanently owns its channel once
started. Do not run multiple orchestration processes against the same serial port.
The repeat runner keeps USB open while issuing reset and requires a changed boot
token after reconnection. These are coordinated resets, not unplugged cold starts.

`run.py --sweep raw` and `--sweep transport` cover 32/128/512/1536/2048-byte frames,
100/1000/5000/10000 offered messages/s and all directions. These are offered rates;
backpressure and submission lateness remain visible. A service sweep is sequential,
not an open-loop outstanding-call window. Do not interpret its offered rate as
achieved throughput. Long saturation runs are unnecessary once a gate has failed.

Kernel fixtures:

| ID | Operation |
|---|---|
| 0 / 1 | Profile creation / braking profile construction |
| 2 / 4 | Segment certification / four-segment serialization and decode |
| 3 / 8 / 9 | Production `PulseCursor`: 50 kHz, reversal, exact 100 kHz boundary |
| 5 / 6 / 7 | Joint planning, Cartesian planning, rejected unreachable Cartesian path |
| 10 / 11 | Experimental certified Newton crossing, ordinary / inlined implementation |
| 12–16 | Nominal three-axis pulse batches covering 1/2/5/10/20 ms at 100 kHz per axis |
| 17 / 18 | Complete three-axis stopping timestamp calculation, production / experimental predictor |
| 19 / 20 | Compact quadratic runs with exact one-tick corrections: construction / expansion |
| 21 / 22 | Fixed-point cubic evaluator: complete three-axis stop / three 256-pulse batches at 100 kHz |
| 23 / 24 | Rate-selected stopping calculation, reference / fixed-point; use `stopping` |

All trajectories are synthetic and uncalibrated; motor enable stays inactive.
`profile` drives STEP pins through the existing HAL, while the kernel fixtures
only compute. The fixed-point solver uses bounded Q32 polynomial comparisons and
falls back to the original double comparison near ambiguous crossings. A 32-bit
Newton ratio is an initial estimate only; the crossing predicate still determines
the output tick. The `executor` fixture uses the production `io::motion::Executor`
and `ProfileSchedule`, including its bounded evaluator and local brake. The other
kernel solvers remain experiments; the existing independent-motor `PulseCursor`
has not been replaced. The reference 100 kHz fixture exposes its existing timing
boundary failure.

## Production executor fixture

See [the executor API and ownership contract](../../src/io/motion/EXECUTOR.md).
The current 12.5 kHz hardware attempt failed on M3 pulse width. Qualification is
stopped; the commands below are reproduction instructions, not passed acceptance
cases. The [result ledger](../../docs/motion-architecture/executor-results.json)
identifies the captured image and incomplete gates.
This fixture exercises the actual production evaluator, executor and HAL; it does
not instantiate powered motor drivers or qualify the future ICC trajectory service.
Preparation and start are measured separately. The M4 executor runs at priority 1,
ICC at 4, its notification worker at 3, and synthetic work at 2. Motion stays
above the work reservation; STEP IRQ precedence is unchanged.
Notification delivery outranks ordinary dispatch even when traffic stays queued.
GUI and ordinary ICC remain active. Load 20 reserves nominal 20% M4 work; acceptance
also checks that at least 95% of its 1 kHz work periods actually ran. Connection
state must remain Connected on both cores. Normal traffic offers 256-byte state
payloads at 50 Hz and 128-byte background payloads at 10 Hz. `--executor-traffic
flood` offers 512 + 480 bytes at 100 Hz; it is a separate overload case, not an
accepted production traffic budget. Reset before entering this fixture if
a previous transport experiment selected different ICC priorities.

```sh
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-full --executor-cycles 1000 --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-scaled --executor-cycles 10 --executor-shape scaled --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-unequal --executor-cycles 10 --executor-shape unequal --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-stop --executor-cycles 1 --executor-stop 1 --stop-at 60 --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-lease --executor-cycles 1 --executor-stop 2 --stop-at 250 --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/executor-stall --executor-cycles 1 --executor-stop 3 --stop-at 60 --load 20
```

The production renewal interval is 7 ms with a 20 ms nominal frontier. M4
worker priorities are motion 1, reserved benchmark work 2, ICC notification 3 and
ICC dispatch 4. The dispatch/notification ordering prevents ordinary traffic from
starving its own wakeups; renewal work must also leave these workers CPU time.
Debug retains symbols and assertions, but benchmark orchestration/route templates
use `-Os`, and the production timing-critical executor/HAL code uses `-O2` in
internal RAM. This tests Debug correctness without making unoptimized benchmark
glue part of the timing workload.

Each output directory is new. The capture compares the full waveform against an
independent polynomial reference, including fractional stopping endpoints. It
checks ENABLE separately, with no glitch filtering. Mode 3 stalls the owner thread;
it deliberately leaves STEP interrupts running. Lost-interrupt cessation is a
separate hardware-guard regression. A debugger halt can expire the ICC session;
reset both cores before continuing after SWD inspection.

## Captures and host tests

```sh
python3 examples/giga_motion_bench/capture.py --output build/my-campaign/step --load 20
python3 examples/giga_motion_bench/capture_stream.py --output build/my-campaign/endurance --seconds 600 --load 20
python3 -m pytest examples/giga_motion_bench/tests -q
cmake --build --preset debug-linux --target motion_bench_metrics_tests
ctest --test-dir build/debug-linux -R '^motion_bench.metrics$' --output-on-failure
python3 examples/giga_motion_bench/repeat.py --output build/my-campaign/repeated
python3 examples/giga_motion_bench/capture_stream.py --output build/my-campaign/profile --profile-cycles 10 --profile-shape equal --load 20
python3 examples/giga_motion_bench/qualify_profiles.py --output build/my-campaign/25k
python3 examples/giga_motion_bench/report.py
python3 examples/giga_motion_bench/reduced_rate_report.py --endurance-dir build/motion-options/endurance-25k-new-cable
MPLCONFIGDIR=/tmp/protomate-matplotlib python3 examples/giga_motion_bench/plot_reduced_rate.py
```

The streaming analyzer examines every captured sample using bounded host memory;
`logic.bin.gz` preserves the original sample stream. Firmware counts alone are not
an electrical test. Output includes clock offset relative to the analyzer, without
assuming which instrument is more accurate. A 24 MHz sample is 41.67 ns.
`capture_stream.py --build build/giga-motion-bench-debug` selects the Debug image
identity. It still uses the same strict electrical checks. For investigation,
`analyze_profile.py capture-directory` writes separate STEP-only evidence and
retains any ENABLE failure; it never turns a rejected capture into `result.json`.

## Measurement contract

- TIM5 is the common 1 MHz, 32-bit timestamp on both cores (71.6-minute modulus).
  Test windows are shorter than the signed half-range. DWT is used only locally.
- Histograms use eight bins per octave: percentiles are **upper bounds**, not exact
  quantiles. Maximum and sum are exact at 1 µs resolution. Small runs do not establish
  meaningful p99.9 confidence or a worst-case guarantee.
- Trace hooks are compiled out of normal images. The benchmark recorder is bounded
  to 512 events/core and uses an interrupt guard. Trace-disabled measurements are
  the performance baseline. `trace` exports happen after the window, with pacing.
- `latency` includes scheduled submission; `processing` excludes submission backlog.
  For services these are round trip on M7, admission on M4. Action M7 `latency`
  means acceptance received and `processing` means terminal result received.
- Raw/transport `backpressure` counts failed send attempts, not discarded frames.
  Topics use the field for intentionally coalesced publications. Flood uses it for
  background-subscription drops. Fixture-specific `value`/`checksum` fields are
  documented by their source and should not be treated as general telemetry.
- CPU ticks have 10 ms resolution; divide by `wall_us`, not the shorter inner
  measurement interval. ISR time is attributed according to the existing runtime
  accounting. `minimum_stack_permill` is the minimum observed registered-thread
  headroom; the static kernel stack also has an explicit fill-pattern scan.
- The synthetic load is a timed worker, not an EtherCAT model. Preemption and
  catch-up affect its marker duty cycle. A `20` setting is **not proof of exactly
  20% exclusive CPU consumption**; do not use it to qualify the EtherCAT reserve.
- For STEP, `maximum_backlog` contains the maximum refill ISR cycles, not queue
  occupancy. `profile` also records the worst pair of `refill_cycles` (240 MHz)
  and `refill_budget_ticks` (10 MHz): the armed guard's remaining deadline before
  refill. Their ratio, `24 * ticks / cycles`, measures each refill against its
  applicable deadline. Taking the longest interrupt from a slow phase and dividing
  the shortest high-speed deadline by it is a separate, conservative bound.
  The counter includes service/refill/callback time; storing the counter itself
  follows the timestamp. Neither margin includes future IPC, retiming or stopping
  preparation. Two 64-pulse halves contain 5.12 ms at 25 kHz; the production executor uses
  two 32-pulse halves for the same 5.12 ms at 12.5 kHz. At lower speed
  they span longer, so this is **not** a globally bounded commitment horizon.

`profile` uses `processing` for prepare/start cost, `scheduling` for one-time
profile/coefficient construction, `received`/`value`/`checksum` for M1/M2/M3
pulse counts and `auxiliary` for synthetic-load iterations. Its load worker and
`fast` fixture 2 use a calibrated fixed instruction count per millisecond,
priority 3, rather than the older wall-time burn in `pulses`. Report the observed
iteration rate too: interrupt masking and scheduling can delay that worker.
It reserves computation, not a real EtherCAT SPI/stack deadline.

`fast` uses an independent allocation-free 32-byte native request/reply queue,
HSEM 6/7 and a readiness notification. Command priorities are M4 1 / M7 2;
ordinary ICC is M4 5 / M7 4, and flood producers are priority 12. Select it before
ordinary ICC construction (otherwise reset). Replies validate the test packet;
they do **not** implement trajectory admission, credits or command cancellation.
The ordinary bridge remains in place for application lifecycle and diagnostics.

## Reserved resources and interpretation

Both cores reserve a private 256 KiB benchmark region in the otherwise unused M4
RAM-debug SDRAM area: M4 `0xC0400000`, M7 `0xC0440000`. Each contains a bounded POD
planner workspace, trace and 24 KiB prepared-timestamp buffer. Explicit linker
assertions prevent overlap. M4/M7 reports occupy SRAM4 at `0x3800D000`, below the
handoff. No measurement arrays or kernel stacks consume the small production UI heap.
Additional explicit reservations are 256 KiB/core for compact-run experiments
(M4 `0xC0480000`, M7 `0xC04C0000`) and 64 KiB shared at `0xC0500000` for the native
command queue. All use the existing normal, non-cacheable benchmark SDRAM mapping.
The production executor reserves 25,408 bytes of M4 D2 SRAM for its profile pool;
ISR code, including the double fallback selected by the linker, stays in instruction
RAM. The separate legacy `profile` comparison fixture now allocates its optional
sequences from AXI heap, and the M4 `kernel` fixture allocates its 20 KiB stack only
when invoked. Their placement differs from archived campaign images: reproduce
those historical measurements with the recorded binaries, not a new build.
The M7 kernel stack retains its static reservation and does not consume UI heap.

The benchmark uses a reduced application composition. Its remaining heap is **not**
the production UI's remaining heap. M7 runtime/ThreadX and kernel ITCM placement
is an explicit benchmark experiment (≤64 KiB); normal firmware retains its existing
placement. M4 TLS support is placed with the existing RAM runtime. Full M4 math
placement and physical STEP fixtures cannot all fit the 190 KiB FAST region at once:
the scalar RAM-kernel measurements and the STEP-enabled batch image are distinct
firmware hashes. General M4 planning kernels execute from QSPI; `FixedBench`,
`ProfileBench` and compact expansion execute from instruction RAM. M7 kernels
execute from ITCM. The new dispatch/transport experiments are not in M4 FAST to
leave room for pulse evaluation. `Kernels.cpp` now receives `-O2` through the
owning target's source properties; the earlier directory-local property did not
apply, so earlier wrapper measurements used `-Os`. Compare archived image hashes
and actual build options rather than treating all measurements as the same image.

The campaign stops candidate promotion at failed computation/admission gates.
Passing these fixtures does not enable a live coordinated controller, prove safe
stream-loss stopping, qualify real EtherCAT, or authorize powered motion.
