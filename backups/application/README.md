# Application reference implementation

This archive preserves `main.cpp`, `system_threads.*`, `control/`, `cli/`, the
application-specific `p2p/thread.*` glue, and the previous `src/CMakeLists.txt`.
The sources are retained for reference during the incremental application rewrite.
This directory is not added to the normal application build.

The maintained p2p library is in [`src/common/p2p`](../../src/common/p2p).
The separate [`backups/p2p`](../p2p) archive remains the earlier p2p implementation.

The `step-test-stm32` bench image explicitly compiles the archived `StepperMotor`,
`MotionProfile` and `MotionSequence` sources to preserve its existing tests until
the replacement control implementation is ready.
