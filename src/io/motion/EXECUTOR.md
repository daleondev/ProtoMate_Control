# Motion execution and ownership

The application API connects the M7 planner to M4 through the existing versioned
trajectory service and action. Native Linux workflows run against the independent
simulated plant. Full GIGA application qualification is still in progress; powered
mechanical commissioning is separate.

## Responsibilities

```mermaid
flowchart LR
    CLI[CLI / future GUI commands] --> Client[M7 Client and 32-entry job history]
    Client --> Plan[M7 axis conversion / Robot planner]
    Plan --> ICC[ICC prepare service and execution action]
    ICC --> Own[M4 Controller: ownership and revision checks]
    Own --> Exec[One local execution worker]
    Exec --> DMA[TIM2 / DMA: three independent outputs]
    Own --> Home[Existing StepperMotor homing workers]
    DMA --> State[Commanded state / feedback topics]
    State --> Client
```

M4 atomically admits one family: raw motors, independent axes, or a whole-robot
job. Raw same-motor buffering remains available. Axis targets are Busy until
previous execution and cleanup finish. Robot reservations cover stationary axes
and every sequential homing stage. Configuration and participating-axis revisions
are checked at reservation, preparation and start. Stop/abort/disable bypass family
exclusion; targeting a member of a robot job affects the complete group.

Execution handles remain in the application-owned client, not CLI stack variables.
Readiness callbacks wake existing workers. Terminal results are retained in a
32-entry history; active handles and cancellation state remain alive through cleanup.
Transport loss is reported as unknown where stopping cannot be confirmed.

The public `ui::motion::Axis<Angle>` / `Axis<Distance>` facades and `Robot` share
this client and registry. They do not own hardware or destroy a job when a CLI
callback returns. Axis commands use joint units; raw motor commands retain shaft
degrees/rpm and their existing same-motor buffering behavior.

## Finite and jog programs

The compact finite contract carries one normalized profile, signed integer motor
step displacements, limits, configuration identity and state revisions. All moving
axes follow `start + displacement * progress`. The planner validates the quantized
joint path as well as the requested path. Endpoints are rounded once. A zero move
completes without arming outputs.

One shared progress brake is scaled onto the participating axes. If the original
profile is already braking to rest, its exact tail is retained. A point command
selects a Cartesian destination but follows joint interpolation; it is not a
straight TCP path. Executable Cartesian toolpaths, blending, live override and
hold/resume remain future work.

| Setting | Value |
|---|---:|
| Maximum per-axis rate | 6,250 pulses/s |
| Microsteps M1 / M2 / M3 | 16 / 16 / 8 |
| Finite duration, including stopping continuation | 90 s |
| UI ownership lease / batched renewal | 250 ms / 50 ms |
| Local execution renewal / committed frontier | 40 ms / 80 ms |
| DMA half | 64 pulses |
| Execution worker priority / stack | 1 / 8 KiB (16 KiB on Linux) |

`io_trajectory` serves inspection, ownership acquisition and program preparation;
`io_trajectory_control` carries batched renewals and stop/abort/release on the
urgent lane. The `io_motion` action starts a prepared program and delivers its
terminal result. `io_motor_state` publishes the latest authoritative state at up
to 20 Hz, independently of command replies.

Jog uses the actual soft-limit destination and rolling timer epochs, retaining
fractional crossing state and one continuous job. It does not fabricate an enormous
destination or repeatedly accelerate from rest. The M4 deadline requests braking
after one second by default; `--continuous` removes that deadline but retains the
soft limit, ownership lease and local fault protection.

Homing uses the IO-configured seek direction (Axis 1/2 backward, Axis 3 forward),
release, slow re-latch and a final release. The latch establishes the coordinate;
subsequent clearance motion is accounted normally. Active inputs reject entry.
Either cam activating outside homing inhibits shared enable, aborts all work and
requires re-referencing after disabled-state recovery. No cam-side identity is
tracked. A cancelled recovery does not clear that requirement.

Robot homing retains all-axis ownership across its configured order and the
subsequent sequential neutral return, encoded in the same bounded home program.
The native configuration uses 0° / −60° / 15 mm and Axis 3 → Axis 1 → Axis 2 for
both phases. Neutral moves use configured limits at 25% velocity. Cancellation
prevents later stages; only completed reference stages establish references.
The timeout is not a measured physical travel limit. Physical neutral/reference
coordinates require a verified robot configuration.

## Scheduling and stopping

Crossings use the common bounded fixed-point evaluator, with the original double
predicate resolving numerically ambiguous cases. Initial prepared buffers and
routine refills are evaluated outside interrupt-disabled sections. Publication
checks revisions, DMA ownership and available lead time. Immutable schedule banks
remain alive while HAL references them.

Initial program compilation also runs outside the controller admission lock.
The reservation protects selected axes while stop, cancellation and lease renewal
remain available. The final binding step rechecks ownership, cancellation,
configuration/state revisions, driver readiness and reference switches before
touching outputs. A cancelled or stale compilation cannot arm hardware.

Slow motion and empty stopping prefixes are supported. A completed LOW channel is
parked before replacing its initial DMA word; STM32's prefetched first word must
not survive a replacement. Timer wrap and rolling jog epochs preserve pulse ordinals.
Physical buffer capacity is distinct from committed time.

Explicit controlled stop or UI lease expiry selects an installed stopping branch;
no new M7 computation is needed. Holding torque is retained where possible. An M4
worker failure or hardware fault instead invokes immediate cessation/guard behavior
and invalidates uncertain references. Reconnect cannot replay or resume a job.

The public local API (`prepareExecution`, `startExecution`, `renewExecution`,
`stopExecution`, `executionStatus`) remains available for tests. Application commands
use the owned preparation/action lifecycle. Raw `motor stop` remains immediate abort.
An axis-targeted abort expands only to a currently owned robot group; a group from
an earlier completed or cancelled job cannot stop unrelated current motors.

## Resources and verification

Execution errors retain their stage. `motion jobs <id>` distinguishes a renewal
deadline, STEP handoff deadline, rejected brake state, brake outside the admitted
path, scaling/crossing failure and still-owned schedule storage. Homing timeout
is distinct from cancellation. Once a controlled stop is accepted, later lease
expiry during deceleration does not overwrite its cause.

The executor retains the first failed construction's cut tick, axis/common-progress
scope, position/velocity/acceleration, limits and remaining distance after releasing
its schedule banks. Controller logs print binary64 inputs with 17 significant
digits for replay. Handoff/deadline logs include build/splice maxima and the target's
errno. Formatting happens in thread context, never in the STEP interrupt; failure
capture itself allocates nothing. Diagnostic formatting/storage remains outside
the timing-critical code budget where possible.

The electrical test image additionally records brake construction, refills,
per-axis crossing generation/staging and final publication against TIM2's 10 MHz
clock. Its fixed eight-record ring allocates nothing during execution. Terminal
logs emit the last and slowest retained transaction, not the worst transaction
over an entire job. Ordinary firmware compiles out these trace hooks. The trace
calibration is reported with the results; the current test image measured about
3.28 microseconds per mark including the calibration loop.

The current 40 ms renewal / 80 ms stopping frontier gives roughly 40 ms between
renewal becoming due and the protected boundary. Keep the 2 ms publication guard;
worker servicing and the local 40 ms keepalive remain independent of replacement
cadence. The UI ownership lease is still 250 ms. A lost UI may therefore wait for
lease expiry and the installed frontier before deceleration; deceleration duration
is additional. Ordinary command responsiveness has a 200 ms objective, pending
measurement of actual onset. No hardware fault path uses this relaxed objective.

Current short coordinated analyzer cases pass: full completion emits 400 pulses
per axis and early controlled stopping emits 266 per axis, with 120–121 samples
HIGH and at most one sample of inter-axis skew at 24 MHz. Independent raw commands
emit 18/18/9 pulses. Physical ENABLE stays inactive. These approximately
711-pulse/s cases do not qualify the 6,250-pulse/s ceiling.

The complete application overlap remains a qualification blocker: a continuous
shoulder jog plus an independent elbow move can expire the ICC heartbeat. The UI
reports the explicit failure and follows its existing shutdown policy. M4 remains
alive; the observed cores are not in HardFault. The exact scheduling or blocking
mechanism has not been isolated. A debugger sample inside reply serialization is
not proof that serialization caused the delay.

Each raw/homing worker reserves its stack at motor construction and reuses it
only after joining its predecessor: 10 KiB on ARM, 16 KiB on Linux. The command
worker uses 20 KiB and the executor 8 KiB on ARM. These are allocations, not newly
measured worst-case stack bounds. Current full-load CPU, stack and latency
qualification remains incomplete.

Telemetry reads the generator-synchronized state without taking the motor's
command mutex, so raw startup cannot stall the ICC context through that mutex.
STEP scratch growth releases the previous temporary workspace before allocating
a larger one. The splice guard serializes its users; active DMA buffers and
installed sequence ownership remain separate. Allocation failure returns an
explicit error without modifying an active channel. A register-model regression
checks growth while a different axis is already emitting pulses.

ICC action-event records store encoded bytes bounded by the 2,048-byte frame
limit. Feedback capacity is reused and terminal bytes remain owned until ACK or
session teardown. Worker exceptions identify the failed stage; allocation errors
report Capacity. Interrupting an explicit client wait leaves the motion alive;
stop notification is synchronized with the wait mutex to prevent a lost wakeup.
Homing checks both stage deadlines and the overall program deadline.

Current outcomes, hashes and limitations are recorded in
[the motion review](../../../docs/motion-current-state/review.md) and its
machine-readable evidence. Motor-disconnected fixtures use synthetic references
and inhibit physical motor enable. No adaptive staging is enabled.

The reusable D2 profile pool has nine 2,816-byte slots: a nominal profile and two
stop banks per axis. It supports one coordinated group or three independent groups
without allocating three copies of the entire pool. M7 planning and POD job history
share the reserved 256 KiB motion SDRAM region. No motion buffer relies on the
nearly full UI heap. Completed bridge requests release their encoded frame storage
while correlation/cancellation records remain alive.

Host tests cover ownership conflicts, stale reservations, cancellation, cleanup,
first-word prefetch, empty-to-active transitions, exact counts, shared stopping,
long finite profiles, rolling jog and completion notification races. The simulator
workflow also exercises homing → stop → homing, concurrent axes, joint/point moves,
jogging, group stopping and history eviction against independently observed plant
coordinates. Manufacturing, powered mechanics and physical homing contacts are not
qualified by synthetic reference inputs or simulated feedback.

Hardware acceptance must record the actual application firmware hashes, analyzer
captures, command/stop latency, refill margin, CPU use and stack/heap headroom.
Prior standalone benchmark measurements are not substituted for this evidence.
No ten-minute application qualification is claimed until the short cases pass.
The configured 6,250-pulse/s ceiling and approximately 200 ms interaction objective
are not, by themselves, measured guarantees for the complete GIGA application.
