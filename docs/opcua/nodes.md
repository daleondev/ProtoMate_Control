# ProtoMate OPC UA node reference

Namespace: `urn:protomate:control` · model `1.0.0`.

Generated from [interface.json](interface.json). NodeIds below are string identifiers in that namespace.

Monitoring is read-only. Commands require local permission, a control generation and increasing request IDs.
A receipt acknowledges submission; follow its operation and execution job for acceptance and completion.

## ProtoMate/System



### `ProtoMate/System/Simulation`

True for the native simulated plant

Read: Boolean.

### `ProtoMate/System/UIState`

UI controller state

Read: Int32 {Starting=0, Ready=1, Stopping=2, Stopped=3, Fault=4}.

### `ProtoMate/System/IOState`

Last reported IO controller state

Read: Int32 {Starting=0, Ready=1, Stopping=2, Stopped=3, Fault=4}.

### `ProtoMate/System/CombinedState`

Combined application state

Read: Int32 {Starting=0, Ready=1, Stopping=2, Stopped=3, Fault=4}.

### `ProtoMate/System/Connected`

ICC connection available

Read: Boolean.

### `ProtoMate/System/Initialized`

Initialization never enables motors

Read: Boolean.

### `ProtoMate/System/Enabled`

Commanded shared motor enable, not proof of energization

Read: Boolean.

### `ProtoMate/System/Readiness`

Actionable application readiness condition

Read: String (≤159 bytes).

### `ProtoMate/System/Boot`

IO boot identity

Read: UInt64.

### `ProtoMate/System/Configuration`

Mechanical configuration identity

Read: UInt64.

### `ProtoMate/System/SnapshotRevision`

Coherent telemetry revision

Read: UInt64.

### `ProtoMate/System/Age`

Age of last telemetry receipt

Read: UInt64; unit `ms`.

### `ProtoMate/System/RemotePermission`

Local GUI/CLI permission; OFF after boot

Read: Boolean.

### `ProtoMate/System/UTCValid`

Whether source timestamps have trustworthy UTC

Read: Boolean.

### `ProtoMate/System/BeginControl`

Explicitly begin a new supervised control generation; local permission must be ON

Access: **command**.

| Argument | Type |
|---|---|
| Return | UInt64 |

### `ProtoMate/System/Heartbeat`

Renew control every 100 ms; list held jobs, pad unused entries with zero

Access: **cleanup**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `held_jobs` | UInt64[8] |
| Return | Boolean |

### `ProtoMate/System/GetOperation`

Read the correlated outcome for this session

Access: **read**.

| Argument | Type |
|---|---|
| `operation` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/System/Initialize`

Initialize disabled drivers; never enables automatically

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/System/Enable`

Explicitly enable initialized drivers

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/System/Disable`

Abort and disable through the reserved urgent path

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/System/Stop`

Controlled axis/robot stop; raw motor stop keeps its existing abort semantics

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/System/Abort`

Immediate cessation; invalidate references as defined by the existing API

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Axes



## ProtoMate/Axes/Axis1

First rotary axis

### `ProtoMate/Axes/Axis1/CommandedPosition`

Commanded joint position

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis1/FeedbackPosition`

Estimated feedback; see provenance and validity

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis1/Velocity`

Reported velocity

Read: Double; unit `deg/s`.

### `ProtoMate/Axes/Axis1/Minimum`

Configured lower soft limit

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis1/Maximum`

Configured upper soft limit

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis1/MaximumSpeed`

Configured velocity limit

Read: Double; unit `deg/s`.

### `ProtoMate/Axes/Axis1/Referenced`

Successful reference established

Read: Boolean.

### `ProtoMate/Axes/Axis1/FeedbackValid`

Feedback validity

Read: Boolean.

### `ProtoMate/Axes/Axis1/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Axes/Axis1/Owner`

Existing execution job ID, zero when idle

Read: UInt64.

### `ProtoMate/Axes/Axis1/MoveRelative`

Relative axis move; native axis units and configured defaults

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis1/MoveAbsolute`

Absolute axis move; requires reference

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis1/Jog`

Timed or supervised held axis jog

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryJog` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis1/Home`

Existing axis reference procedure

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis1/Stop`

Controlled stop of axis or owning coordinated group

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Axes/Axis2

Second rotary axis

### `ProtoMate/Axes/Axis2/CommandedPosition`

Commanded joint position

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis2/FeedbackPosition`

Estimated feedback; see provenance and validity

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis2/Velocity`

Reported velocity

Read: Double; unit `deg/s`.

### `ProtoMate/Axes/Axis2/Minimum`

Configured lower soft limit

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis2/Maximum`

Configured upper soft limit

Read: Double; unit `deg`.

### `ProtoMate/Axes/Axis2/MaximumSpeed`

Configured velocity limit

Read: Double; unit `deg/s`.

### `ProtoMate/Axes/Axis2/Referenced`

Successful reference established

Read: Boolean.

### `ProtoMate/Axes/Axis2/FeedbackValid`

Feedback validity

Read: Boolean.

### `ProtoMate/Axes/Axis2/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Axes/Axis2/Owner`

Existing execution job ID, zero when idle

Read: UInt64.

### `ProtoMate/Axes/Axis2/MoveRelative`

Relative axis move; native axis units and configured defaults

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis2/MoveAbsolute`

Absolute axis move; requires reference

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis2/Jog`

Timed or supervised held axis jog

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RotaryJog` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis2/Home`

Existing axis reference procedure

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis2/Stop`

Controlled stop of axis or owning coordinated group

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Axes/Axis3

Linear axis

### `ProtoMate/Axes/Axis3/CommandedPosition`

Commanded joint position

Read: Double; unit `mm`.

### `ProtoMate/Axes/Axis3/FeedbackPosition`

Estimated feedback; see provenance and validity

Read: Double; unit `mm`.

### `ProtoMate/Axes/Axis3/Velocity`

Reported velocity

Read: Double; unit `mm/s`.

### `ProtoMate/Axes/Axis3/Minimum`

Configured lower soft limit

Read: Double; unit `mm`.

### `ProtoMate/Axes/Axis3/Maximum`

Configured upper soft limit

Read: Double; unit `mm`.

### `ProtoMate/Axes/Axis3/MaximumSpeed`

Configured velocity limit

Read: Double; unit `mm/s`.

### `ProtoMate/Axes/Axis3/Referenced`

Successful reference established

Read: Boolean.

### `ProtoMate/Axes/Axis3/FeedbackValid`

Feedback validity

Read: Boolean.

### `ProtoMate/Axes/Axis3/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Axes/Axis3/Owner`

Existing execution job ID, zero when idle

Read: UInt64.

### `ProtoMate/Axes/Axis3/MoveRelative`

Relative axis move; native axis units and configured defaults

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/LinearMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis3/MoveAbsolute`

Absolute axis move; requires reference

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/LinearMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis3/Jog`

Timed or supervised held axis jog

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/LinearJog` |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis3/Home`

Existing axis reference procedure

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/Axes/Axis3/Stop`

Controlled stop of axis or owning coordinated group

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Robot



### `ProtoMate/Robot/Joints`

Estimated feedback joints: degrees, degrees, millimetres

Read: Double[3].

### `ProtoMate/Robot/EstimatedTCP`

Mixed encoder/driver-phase estimate; not fully measured TCP

Read: Double[3]; unit `mm`.

### `ProtoMate/Robot/DerivedYaw`

Derived yaw, not an independent axis

Read: Double; unit `deg`.

### `ProtoMate/Robot/FeedbackValid`

Configuration, reference and feedback are valid

Read: Boolean.

### `ProtoMate/Robot/Configured`

Geometry/workspace configuration is present

Read: Boolean.

### `ProtoMate/Robot/Jog`

Existing Cartesian velocity jog; timed or supervised held motion

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/CartesianJog` |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/Home`

Reference all axes in the configured sequence

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/Stop`

Stop the owning robot job

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/MoveJointsRelative`

Existing application JointRelative operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/MoveJointsAbsolute`

Existing application JointAbsolute operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/MovePointRelative`

Existing application PointRelative operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Robot/MovePointAbsolute`

Existing application PointAbsolute operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

## ProtoMate/Motors



## ProtoMate/Motors/M1



### `ProtoMate/Motors/M1/CommandedPosition`

Shaft degrees

Read: Double; unit `deg`.

### `ProtoMate/Motors/M1/FeedbackPosition`

Shaft feedback degrees; validity published separately

Read: Double; unit `deg`.

### `ProtoMate/Motors/M1/Velocity`

Shaft speed

Read: Double; unit `rpm`.

### `ProtoMate/Motors/M1/Driver`

Configured driver type

Read: String (≤31 bytes).

### `ProtoMate/Motors/M1/Ready`

Reported driver readiness

Read: Boolean.

### `ProtoMate/Motors/M1/Fault`

Active or latched driver fault

Read: Boolean.

### `ProtoMate/Motors/M1/FaultCode`

Typed driver fault code

Read: UInt32.

### `ProtoMate/Motors/M1/Warning`

Reported driver warning

Read: Boolean.

### `ProtoMate/Motors/M1/ReferenceSwitch`

Reference/hard-limit input

Read: Boolean.

### `ProtoMate/Motors/M1/FeedbackValid`

Feedback is valid

Read: Boolean.

### `ProtoMate/Motors/M1/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Motors/M1/Owner`

Existing job ID

Read: UInt64.

### `ProtoMate/Motors/M1/ReadDefaults`

Cached raw motor dynamics; no IO request

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/MotorDefaults` |

### `ProtoMate/Motors/M1/MoveRelative`

Existing raw motor MotorRelative semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M1/MoveAbsolute`

Existing raw motor MotorAbsolute semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M1/Home`

Existing raw motor MotorHome semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M1/SetSpeed`

Existing raw motor MotorSpeed semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M1/SetDefaults`

Existing raw motor MotorDefaults semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M1/Abort`

Immediate raw motor abort and existing reference invalidation

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Motors/M2



### `ProtoMate/Motors/M2/CommandedPosition`

Shaft degrees

Read: Double; unit `deg`.

### `ProtoMate/Motors/M2/FeedbackPosition`

Shaft feedback degrees; validity published separately

Read: Double; unit `deg`.

### `ProtoMate/Motors/M2/Velocity`

Shaft speed

Read: Double; unit `rpm`.

### `ProtoMate/Motors/M2/Driver`

Configured driver type

Read: String (≤31 bytes).

### `ProtoMate/Motors/M2/Ready`

Reported driver readiness

Read: Boolean.

### `ProtoMate/Motors/M2/Fault`

Active or latched driver fault

Read: Boolean.

### `ProtoMate/Motors/M2/FaultCode`

Typed driver fault code

Read: UInt32.

### `ProtoMate/Motors/M2/Warning`

Reported driver warning

Read: Boolean.

### `ProtoMate/Motors/M2/ReferenceSwitch`

Reference/hard-limit input

Read: Boolean.

### `ProtoMate/Motors/M2/FeedbackValid`

Feedback is valid

Read: Boolean.

### `ProtoMate/Motors/M2/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Motors/M2/Owner`

Existing job ID

Read: UInt64.

### `ProtoMate/Motors/M2/ReadDefaults`

Cached raw motor dynamics; no IO request

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/MotorDefaults` |

### `ProtoMate/Motors/M2/MoveRelative`

Existing raw motor MotorRelative semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M2/MoveAbsolute`

Existing raw motor MotorAbsolute semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M2/Home`

Existing raw motor MotorHome semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M2/SetSpeed`

Existing raw motor MotorSpeed semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M2/SetDefaults`

Existing raw motor MotorDefaults semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M2/Abort`

Immediate raw motor abort and existing reference invalidation

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Motors/M3



### `ProtoMate/Motors/M3/CommandedPosition`

Shaft degrees

Read: Double; unit `deg`.

### `ProtoMate/Motors/M3/FeedbackPosition`

Shaft feedback degrees; validity published separately

Read: Double; unit `deg`.

### `ProtoMate/Motors/M3/Velocity`

Shaft speed

Read: Double; unit `rpm`.

### `ProtoMate/Motors/M3/Driver`

Configured driver type

Read: String (≤31 bytes).

### `ProtoMate/Motors/M3/Ready`

Reported driver readiness

Read: Boolean.

### `ProtoMate/Motors/M3/Fault`

Active or latched driver fault

Read: Boolean.

### `ProtoMate/Motors/M3/FaultCode`

Typed driver fault code

Read: UInt32.

### `ProtoMate/Motors/M3/Warning`

Reported driver warning

Read: Boolean.

### `ProtoMate/Motors/M3/ReferenceSwitch`

Reference/hard-limit input

Read: Boolean.

### `ProtoMate/Motors/M3/FeedbackValid`

Feedback is valid

Read: Boolean.

### `ProtoMate/Motors/M3/EncoderFeedback`

Encoder if true; driver-phase estimate otherwise

Read: Boolean.

### `ProtoMate/Motors/M3/Owner`

Existing job ID

Read: UInt64.

### `ProtoMate/Motors/M3/ReadDefaults`

Cached raw motor dynamics; no IO request

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/MotorDefaults` |

### `ProtoMate/Motors/M3/MoveRelative`

Existing raw motor MotorRelative semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M3/MoveAbsolute`

Existing raw motor MotorAbsolute semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M3/Home`

Existing raw motor MotorHome semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M3/SetSpeed`

Existing raw motor MotorSpeed semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M3/SetDefaults`

Existing raw motor MotorDefaults semantics; shaft units

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/MotorMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Motors/M3/Abort`

Immediate raw motor abort and existing reference invalidation

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Planning



### `ProtoMate/Planning/PreviewJoints`

Existing application PreviewJoint operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Planning/PreviewPoint`

Existing application PreviewPoint operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Planning/PreviewLine`

Existing application PreviewLine operation; no automatic preemption

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `motion` | `Types/RobotMove` |
| Return | `Types/Receipt` |

### `ProtoMate/Planning/ReadPage`

At most eight preview records; previews are not execution jobs

Access: **read**.

| Argument | Type |
|---|---|
| `offset` | UInt32 |
| Return | `Types/JobPage` |

### `ProtoMate/Planning/ReadResult`

Preview endpoints and certified bounds; never executable, motor peaks in pulse units

Access: **read**.

| Argument | Type |
|---|---|
| `job` | UInt64 |
| Return | `Types/PreviewResult` |

### `ProtoMate/Planning/Cancel`

Cancel an existing preview

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `job` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Jobs



### `ProtoMate/Jobs/ReadPage`

At most eight existing execution jobs per page

Access: **read**.

| Argument | Type |
|---|---|
| `offset` | UInt32 |
| Return | `Types/JobPage` |

### `ProtoMate/Jobs/Cancel`

Cancel preparation or request the owning job's existing stop behavior

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `job` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/IO



### `ProtoMate/IO/ReferenceSwitches`

Active reference/hard-limit inputs, ordered M1–M3

Read: Boolean[3].

### `ProtoMate/IO/DriverFaults`

Reported/latched driver faults, ordered M1–M3

Read: Boolean[3].

### `ProtoMate/IO/DriverWarnings`

Reported driver warnings, ordered M1–M3

Read: Boolean[3].

### `ProtoMate/IO/DriverReady`

Reported readiness, not proof of energization

Read: Boolean[3].

### `ProtoMate/IO/EnableCommand`

Commanded shared enable; not independent power feedback

Read: Boolean.

## ProtoMate/Messages



### `ProtoMate/Messages/Revision`

Shared message history revision

Read: UInt64.

### `ProtoMate/Messages/ReadPage`

Latest eight shared application messages, newest first; times are monotonic

Access: **read**.

| Argument | Type |
|---|---|
| `offset` | UInt32 |
| Return | `Types/MessagePage` |

### `ProtoMate/Messages/Acknowledge`

Acknowledge history through sequence; never clears underlying hardware faults

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `through_sequence` | UInt64 |
| Return | `Types/Receipt` |

## ProtoMate/Settings



### `ProtoMate/Settings/Brightness`

Applied backlight percentage

Read: UInt32.

### `ProtoMate/Settings/ReadConfiguration`

Cached mechanical configuration and calibration; unavailable when unconfigured

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/MechanicalConfiguration` |

### `ProtoMate/Settings/SetBrightness`

Enqueue display brightness 0–100 percent; readback reports applied value

Access: **command**.

| Argument | Type |
|---|---|
| `generation` | UInt64 |
| `request` | UInt64 |
| `percent` | UInt32 |
| Return | `Types/Receipt` |

## ProtoMate/Diagnostics



### `ProtoMate/Diagnostics/OpcUaCapacity`

Reserved OPC UA arena capacity

Read: UInt64; unit `B`.

### `ProtoMate/Diagnostics/OpcUaUsed`

Current arena allocation including allocator overhead

Read: UInt64; unit `B`.

### `ProtoMate/Diagnostics/OpcUaPeak`

Peak arena allocation

Read: UInt64; unit `B`.

### `ProtoMate/Diagnostics/OpcUaAllocationFailures`

Failed arena allocation requests

Read: UInt64.

### `ProtoMate/Diagnostics/ReadQueues`

Bounded ordinary/urgent application admission; full queues return explicit rejection

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/FrontendQueues` |

### `ProtoMate/Diagnostics/ReadNetwork`

Cached network state and bounded allocator/packet-pool diagnostics; excludes credentials

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/NetworkDiagnostics` |

### `ProtoMate/Diagnostics/RefreshMemory`

Queue an asynchronous UI/IO heap or stack snapshot; false means another query is pending

Access: **read**.

| Argument | Type |
|---|---|
| `io` | Boolean |
| `stacks` | Boolean |
| Return | Boolean |

### `ProtoMate/Diagnostics/ReadGraphics`

Read cached GUI and renderer counters; no rendering work

Access: **read**.

| Argument | Type |
|---|---|
| Return | `Types/Graphics` |

### `ProtoMate/Diagnostics/ReadPage`

Cached UI/IO memory snapshot; no implicit IO query

Access: **read**.

| Argument | Type |
|---|---|
| `offset` | UInt32 |
| Return | `Types/DiagnosticPage` |

## ProtoMate/Capabilities



### `ProtoMate/Capabilities/ModelVersion`

Version of the frozen node-ID manifest

Read: String (≤31 bytes).

### `ProtoMate/Capabilities/Programs`

Program execution is unavailable

Read: Boolean.

### `ProtoMate/Capabilities/Presets`

Saved-position management is unavailable

Read: Boolean.

### `ProtoMate/Capabilities/MinimumSamplingInterval`

Minimum supported sampling interval

Read: UInt32; unit `ms`.

### `ProtoMate/Capabilities/MaximumSessions`

Active session capacity

Read: UInt32.

### `ProtoMate/Capabilities/ResourceLimits`

Channels, subscriptions, items, pending commands, job history, page rows, chunk bytes, message bytes, heartbeat ms, expiry ms

Read: UInt32[10].

## `Types/Receipt`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `operation` | UInt64 |  |  |
| `job` | UInt64 |  |  |
| `submitted` | Boolean |  |  |
| `phase` | Int32 {Submitted=0, Completed=1, Rejected=2, Unknown=3} |  |  |
| `reason` | String (≤159 bytes) |  |  |

## `Types/RotaryMove`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `position` | Double | deg [-1000000.0, 1000000.0] | Target or displacement |
| `speed` | Double | deg/s [0, 1000000.0] | Zero selects configured default |
| `acceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects configured default |
| `deceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects configured default |
| `jerk` | Double |  [0, 1000000000000000.0] | Joint units / s^3; zero selects configured default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects configured default |

## `Types/RotaryJog`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `speed` | Double | deg/s [-1000000.0, 1000000.0] | Signed joint velocity |
| `duration` | Double |  [0, 3600] | Seconds before deceleration is requested |
| `held` | Boolean |  | Requires explicit held-job heartbeat supervision |
| `acceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects default |
| `deceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects default |
| `jerk` | Double |  [0, 1000000000000000.0] | Joint units / s^3; zero selects default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects default |

## `Types/LinearMove`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `position` | Double | mm [-1000000.0, 1000000.0] | Target or displacement |
| `speed` | Double | mm/s [0, 1000000.0] | Zero selects configured default |
| `acceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects configured default |
| `deceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects configured default |
| `jerk` | Double |  [0, 1000000000000000.0] | Joint units / s^3; zero selects configured default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects configured default |

## `Types/LinearJog`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `speed` | Double | mm/s [-1000000.0, 1000000.0] | Signed joint velocity |
| `duration` | Double |  [0, 3600] | Seconds before deceleration is requested |
| `held` | Boolean |  | Requires explicit held-job heartbeat supervision |
| `acceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects default |
| `deceleration` | Double |  [0, 1000000000000.0] | Joint units / s^2; zero selects default |
| `jerk` | Double |  [0, 1000000000000000.0] | Joint units / s^3; zero selects default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects default |

## `Types/CartesianJog`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `direction` | Double[3] |  [-1, 1] | Cartesian direction, normalized by the existing planner |
| `speed` | Double |  [0, 1] | Configured velocity factor |
| `duration` | Double |  [0, 3600] | Seconds before deceleration is requested |
| `held` | Boolean |  | Requires held-job heartbeat supervision |
| `acceleration` | Double |  [0, 1] | Configured dynamics factors; zero selects default |
| `deceleration` | Double |  [0, 1] | Configured dynamics factors; zero selects default |
| `jerk` | Double |  [0, 1] | Configured dynamics factors; zero selects default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects default |

## `Types/RobotMove`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `position` | Double[3] |  [-1000000.0, 1000000.0] | Joints: degrees/degrees/mm; point: mm/mm/mm |
| `speed` | Double |  [0, 1] | Configured velocity factor; zero selects default |
| `acceleration` | Double |  [0, 1] | Configured acceleration factor; zero selects default |
| `deceleration` | Double |  [0, 1] | Configured deceleration factor; zero selects default |
| `jerk` | Double |  [0, 1] | Configured jerk factor; zero selects default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects default |

## `Types/MotorDefaults`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `available` | Boolean |  |  |
| `acceleration` | Double |  | Shaft rpm/s |
| `deceleration` | Double |  | Shaft rpm/s |
| `jerk` | Double |  | Shaft rpm/s squared |

## `Types/MotorMove`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `position` | Double | deg [-1000000000.0, 1000000000.0] | Shaft target or displacement |
| `speed` | Double | rpm [-1000000.0, 1000000.0] | Shaft velocity |
| `acceleration` | Double |  [0, 1000000000000.0] | Shaft rpm/s; zero selects default |
| `deceleration` | Double |  [0, 1000000000000.0] | Shaft rpm/s; zero selects default |
| `jerk` | Double |  [0, 1000000000000000.0] | Shaft rpm/s^2; zero selects default |
| `timeout` | Double |  [0, 3600] | Seconds; zero selects default |
| `latch` | Double | rpm [0, 1000000.0] | Slow latch speed for homing |
| `buffer` | UInt32 |  [0, 5] | Existing MotorBuffer enum, 0 through 5 |
| `confirm_replacement` | Boolean |  | Explicitly confirm replacement; cannot replace another source's queue |

## `Types/JobPage`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `offset` | UInt32 |  |  |
| `count` | UInt32 |  |  |
| `records` | `Types/Job`[8] |  |  |

## `Types/PreviewResult`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `available` | Boolean |  |  |
| `state` | UInt32 |  |  |
| `error` | UInt32 |  |  |
| `segments` | UInt32 |  |  |
| `id` | UInt64 |  |  |
| `source` | UInt64 |  |  |
| `duration` | Double |  | Seconds |
| `approximation_mm` | Double | mm | Separate approximation and step-quantization bounds |
| `quantization_mm` | Double | mm | Separate approximation and step-quantization bounds |
| `endpoint_joints` | Double[3] |  | Axis 1/2 degrees, Axis 3 millimetres |
| `endpoint_tcp` | Double[3] | mm | Work-frame TCP |
| `peak_velocity` | Double[3] |  | M1/M2/M3 pulses per second |
| `peak_acceleration` | Double[3] |  | M1/M2/M3 pulses per second squared |
| `peak_jerk` | Double[3] |  | M1/M2/M3 pulses per second cubed |

## `Types/MessagePage`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `count` | UInt32 |  |  |
| `revision` | UInt64 |  |  |
| `records` | `Types/ApplicationMessage`[8] |  |  |

## `Types/MechanicalConfiguration`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `available` | Boolean |  |  |
| `calibrated` | Boolean |  |  |
| `simulation` | Boolean |  |  |
| `kinematics` | Boolean |  |  |
| `neutral_available` | Boolean |  |  |
| `identity` | UInt64 |  |  |
| `hardware` | UInt64 |  |  |
| `motor_for_axis` | UInt32[3] |  | One-based axis/motor indices |
| `home_order` | UInt32[3] |  | One-based axis/motor indices |
| `opposite` | Boolean[3] |  |  |
| `home_configured` | Boolean[3] |  |  |
| `home_positive` | Boolean[3] |  |  |
| `reduction` | Double[3] |  | Motor revolutions per output revolution |
| `reference` | Double[3] |  | Axis 1/2 degrees, Axis 3 millimetres |
| `minimum` | Double[3] |  | Axis 1/2 degrees, Axis 3 millimetres |
| `maximum` | Double[3] |  | Axis 1/2 degrees, Axis 3 millimetres |
| `neutral` | Double[3] |  | Axis 1/2 degrees, Axis 3 millimetres |
| `velocity` | Double[3] |  | Joint units per second |
| `acceleration` | Double[3] |  | Joint units per second squared |
| `deceleration` | Double[3] |  | Joint units per second squared |
| `jerk` | Double[3] |  | Joint units per second cubed |
| `motor_step_degrees` | Double[3] | deg | M1/M2/M3 electrical step angles |
| `base_mm` | Double[3] | mm | Millimetres |
| `tool_mm` | Double[3] | mm | Millimetres |
| `work_mm` | Double[3] | mm | Millimetres |
| `workspace_min_mm` | Double[3] | mm | Millimetres |
| `workspace_max_mm` | Double[3] | mm | Millimetres |
| `home_seek_rpm` | Double[3] | rpm | Shaft rpm |
| `home_latch_rpm` | Double[3] | rpm | Shaft rpm |
| `home_timeout_s` | Double[3] |  | Seconds |
| `links_mm` | Double[2] | mm | Arm lengths |
| `screw_lead_mm` | Double |  | Millimetres per screw revolution |
| `base_yaw_deg` | Double | deg | Degrees |
| `tool_yaw_deg` | Double | deg | Degrees |
| `work_yaw_deg` | Double | deg | Degrees |
| `approximation_mm` | Double | mm | Numerical approximation budget; excludes quantization and mechanics |

## `Types/FrontendQueues`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `pending` | UInt32 |  |  |
| `urgent` | UInt32 |  |  |
| `capacity` | UInt32 |  |  |
| `urgent_capacity` | UInt32 |  |  |
| `peak` | UInt32 |  |  |
| `rejected` | UInt64 |  |  |

## `Types/NetworkDiagnostics`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `wifi` | Boolean |  |  |
| `connected` | Boolean |  |  |
| `busy` | Boolean |  |  |
| `saved` | Boolean |  |  |
| `state` | UInt32 |  | Network manager state |
| `address` | UInt32 |  | IPv4 in host integer form, most significant octet first |
| `error` | Int32 |  | Driver result |
| `capacity` | UInt64 | B | Dedicated network allocator bytes |
| `used` | UInt64 | B | Dedicated network allocator bytes |
| `peak` | UInt64 | B | Dedicated network allocator bytes |
| `failures` | UInt64 |  |  |
| `packets` | UInt64 |  |  |
| `available_packets` | UInt64 |  |  |
| `empty_packet_requests` | UInt64 |  |  |
| `packet_suspensions` | UInt64 |  |  |
| `invalid_packet_releases` | UInt64 |  |  |
| `receive_packets` | UInt64 |  |  |
| `receive_available` | UInt64 |  |  |
| `receive_empty_requests` | UInt64 |  |  |

## `Types/Graphics`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `available` | Boolean |  |  |
| `brightness` | UInt32 |  |  |
| `frames` | UInt64 |  |  |
| `presented` | UInt64 |  |  |
| `underruns` | UInt64 |  |  |
| `dma_errors` | UInt64 |  |  |
| `render_failures` | UInt64 |  |  |
| `queue_overflow` | UInt64 |  |  |
| `maximum_render_us` | UInt64 |  |  |
| `scene_presented` | UInt64 |  |  |
| `scene_latency_us` | UInt64 |  |  |

## `Types/DiagnosticPage`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `count` | UInt32 |  |  |
| `total` | UInt32 |  |  |
| `omitted` | UInt32 |  |  |
| `allocator` | UInt32 |  |  |
| `available` | Boolean |  |  |
| `busy` | Boolean |  |  |
| `io` | Boolean |  |  |
| `stacks` | Boolean |  |  |
| `shared` | Boolean |  |  |
| `age` | UInt64 |  |  |
| `capacity` | UInt64 |  |  |
| `used` | UInt64 |  |  |
| `error` | String (≤47 bytes) |  |  |
| `rows` | `Types/DiagnosticRow`[6] |  |  |

## `Types/Job`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `id` | UInt64 |  |  |
| `source` | UInt64 |  |  |
| `state` | UInt32 |  |  |
| `family` | UInt32 |  |  |
| `axes` | UInt32 |  |  |
| `terminal` | Boolean |  |  |
| `preview` | Boolean |  |  |
| `busy_owner` | UInt64 |  |  |
| `duration` | Double |  |  |
| `endpoint_steps` | Double[3] |  | Quantized M1/M2/M3 pulse positions |
| `requested` | Double[3] |  | Requested values in the accompanying units and coordinate space |
| `requested_space` | String (≤31 bytes) |  |  |
| `requested_units` | String (≤31 bytes) |  |  |
| `relative` | Boolean |  |  |
| `detail` | String (≤95 bytes) |  |  |

## `Types/ApplicationMessage`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `sequence` | UInt64 |  |  |
| `time_ms` | UInt64 |  |  |
| `operation` | UInt64 |  |  |
| `severity` | Int32 {Info=0, Warning=1, Error=2} |  |  |
| `acknowledged` | Boolean |  |  |
| `text` | String (≤159 bytes) |  |  |

## `Types/DiagnosticRow`

| Field | Type | Unit / bounds | Meaning |
|---|---|---|---|
| `name` | String (≤31 bytes) |  |  |
| `values` | UInt64[4] |  |  |
