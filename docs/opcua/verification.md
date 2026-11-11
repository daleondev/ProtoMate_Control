# Integration qualification — remote-motion endurance blocked

Recorded 2026-11-11/12. **The interface is implemented, but sustained GIGA Wi-Fi motion control is not release-qualified.** Short electrical workflows, monitoring, capacity and recovery pass. The ten-minute combined-motion acceptance gate did not pass because the remote control generation expired during network/server latency spikes. The 500 ms deadline has not been relaxed.

## Builds and host tests

- Linux Debug/Release and GIGA M4/M7 Debug/Release application builds pass. The relocated standalone sample builds in both configurations on both platforms.
- Full Linux CTest: **87/87 pass**. Separate reviewed GUI golden run: **69 cases pass**, including Settings → Network and masked password entry.
- Targeted executor/HAL/STM32 register-model/remote-control regressions pass after the final scratch-allocation fix.
- Six final OPC UA/schema/native-plant tests pass (176.96 s). The real workflow exercises homing → stop → homing, axis moves, coordinated joints, point moves, Cartesian timed jog, independent axes, raw/axis Busy exclusion, held-input release and source expiry. Joint/TCP changes are checked against the simulated mechanism. Separate tests cover reflected types/bounds, permission, duplicate requests, diagnostics and server teardown.
- The existing straight-line preview fixture rejects jerk certification (`JerkLimit`). That typed rejection remains visible; the remote interface does not weaken the planner or make line previews executable.
- No clang-tidy or sanitizer campaign was run for this integration.

## GIGA monitoring, capacity and recovery

Tests run alongside the real GUI, CLI, ICC and application workers:

| Check | Result |
|---|---|
| External SDRAM access/initialization with D-cache enabled | Pass |
| Two sessions, four subscriptions, 64 monitored items | Pass; all initial notifications received |
| Two additional transient discovery channels | Pass |
| Third session and 65th monitored item | Explicit capacity errors; existing clients remain usable |
| Interrupted TCP, incomplete HEL, fragmented messages | Pass |
| Typed reads through fragmenting TCP proxy | Pass |
| Shutdown while client stops reading | Pass; server arena returns to zero |
| Server restart / controlled Wi-Fi disconnect and reconnect | Pass; permission stays OFF |
| Arena use after slow reader, reconnect and restart | Identical: 406,080 B with test session |
| Unexpected LTDC underruns / DMA errors / render failures | Zero |
| Sustained Wi-Fi motion with 500 ms source deadline | **Blocked; see below** |

Actual RF access-point interruption was not tested; the user selected controlled disconnect/reconnect only. A successful controlled reconnect is not evidence of RF-failure detection latency.

Measured resource figures after capacity, motion screens and recovery:

| Resource | Measurement |
|---|---:|
| UA arena peak | 456,648 / 524,288 B (64-bit Linux uses its separate 1 MiB budget) |
| UA allocation failures | 0 |
| Network vendor allocator | ~26 KiB used; 128 KiB allocator inside the 256 KiB reservation |
| TX / RX packet pools | 11 packets each; both recover to all available |
| OPC UA worker stack | 10,280 / 14,332 usable B; **28.3% headroom** |
| Network manager stack | 4,160 / 8,188 usable B; **49.2% headroom** |
| GUI worker stack | 4,312 / 16,380 usable B |
| Planner worker stack | 14,704 / 24,572 usable B |
| UI general heap available | 26,784 B at the recorded post-test snapshot |
| IO general heap available | 32,184 B at the recorded post-test snapshot |
| M7 linked ordinary DTCM | 112,392 / 112,640 B; **248 B remaining** |
| UI snapshot/byte-buffer reservation linked | 84,800 / 131,072 B, Release |

Reserved arenas appear 100% allocated in linker reports by design; runtime peaks are reported separately. Internal DTCM is nearly full. Future worker/state growth needs an explicit placement decision, not general-heap fallback. No complete steady-state memory/CPU qualification is claimed because the endurance gate stopped early.

## Motor-disconnected electrical evidence

The explicit electrical fixture substitutes synthetic driver/reference/configuration inputs and physically keeps enable inactive. It uses the real remote frontend, ICC, executor and timer/DMA backend. This does not establish calibrated physical motion or real-driver feedback.

| Capture | Observed STEP counts (M1 / M2 / M3) | Result |
|---|---|---|
| Raw relative moves | 18 / 18 / 9 | Exact; complete pulses |
| Coordinated move | 400 / 400 / 400 | Exact; common timeline within one 24 MHz sample |
| Controlled jog stop | 41 / 0 / 0 | Stopped; complete final pulse |
| Remote heartbeat loss | 52 / 0 / 0 | Local stopping; no automatic resume |
| First post-fix re-home/move screen | 2,288 / 2,288 / 2,288 | Exact |
| Second re-home/move screen, same boot | 1,902 / 1,902 / 1,902 | Exact |
| Periodic-heartbeat 30 s screen | 3,100 / 3,100 / 3,100 | Exact |

Every ENABLE sample in passing captures remained inactive. Pulse highs were 120–121 samples (~5 µs). The coordinated capture's maximum inter-axis rising-edge spread was one sample (41.7 ns), including analyzer quantization. Current wiring observes STEP and enable, **not DIR**; no new direction-margin qualification is claimed.

Submission receipt round trips in one six-move screen ranged approximately 24–141 ms. These are client-to-receipt times, not IO acceptance, motion-onset or physical stop latency. GUI remained responsive to the exercised CLI/state operations, with zero presentation errors; the synthetic fixture did not provide valid live mechanical feedback, so the 3D scene remained nominal/static. This is ordinary GUI coexistence, not a new moving-3D performance result.

## Fixed defects

1. **NetX passive-open lifecycle:** repeated nonblocking accept after an abandoned handshake reused cleared peer addresses, reached a checksum assertion and stranded the IP mutex. Accept now arms once; invalid listening states retire under the IP lock. Host regressions and real interrupted connections pass.
2. **Network timer scaling:** `NX_IP_PERIODIC_RATE` now matches the application's 1 kHz ThreadX tick instead of the Cortex-M port's 100 Hz default. Motion timing is unchanged.
3. **Held-jog completion race:** a heartbeat may still contain a known terminal gesture. It is ignored without revival; other active jobs remain supervised. Unknown/foreign/expired active gestures still reject renewal.
4. **M4 scratch fragmentation after re-homing:** a failed 9,216 B contiguous allocation was recorded despite ~19 KiB aggregate heap headroom. Scratch is now allocated per axis, with the same payload size, evaluator and timing. Repeated re-homing and subsequent motion screens pass; the final fixture recorded zero allocation failures.
5. **False Busy detail:** preparation errors carried their own job ID, which the frontend incorrectly displayed as an owning Busy job. Only actual Busy errors now populate that field.

## Remaining blocker and bounded stopping point

The first intended endurance run rejected its first post-re-home motion because of scratch allocation. That was reproduced, fixed and screened before further endurance work.

The next run progressed through more than 30 coordinated commands, then a **720 ms heartbeat round trip** caused the request/reply client to miss the 500 ms source deadline. The generation expired and further motion was rejected. GUI/ICC stayed Ready, with no display errors or allocation failures.

A bounded periodic client was then tested, allowing at most eight outstanding heartbeat replies. Its 30-second screen passed. The final endurance attempt reproduced expiry during initial homing/preparation: submitted heartbeat spacing remained ≤144 ms, but replies reached **758 ms** and six renewals were rejected after expiry. Thus waiting synchronously for replies was not the whole explanation. The exact delay location—radio/TCP delivery versus server scheduling/processing—has **not** been isolated. Round-trip time alone cannot locate it or establish one-way delay.

No further motion endurance runs were launched. There is no successful ten-minute motion result, no weakened deadline and no automatic generation reopening/replay. The existing expiry behavior is doing its job, but nuisance stops/rejections prevent qualification as dependable remote motion control.

The next focused investigation should correlate timestamped client sends with server receipt and motion-client renewal processing, alongside TCP retransmission/window evidence. It must distinguish network delivery from scheduling before proposing a transport, priority or deadline change. Local GUI/CLI motion and read-only OPC UA remain usable; remote permission stays OFF in the restored normal firmware.

## Reproducibility

- [Capture results, hashes and failed-run summaries](evidence/captures.json)
- [Final Release image manifest](evidence/giga-release-manifest.json), pair `202796ba7d72282d…`
- [Debug image manifest](evidence/giga-debug-manifest.json), pair `faf2da48516b0a9e…`
- [Final electrical-fixture manifest](evidence/giga-motion-api-release-manifest.json), pair `8964f0ae29f0d86d…`
- [Full host regression log](evidence/final-host-regressions.log)
- [Final targeted regression log](evidence/fragment-scratch-host.log)
- [Capacity and stack measurements](evidence/capacity-final.log)
- [Recovery resource values](evidence/recovery.json)
- [Source hashes relative to the recorded base commit](evidence/source-hashes.json)

Earlier short captures used fixture pair `966a7d1d25890218…`; re-home and periodic screens used final fixture `8964f0ae29f0d86d…`. Unfiltered gzip analyzer streams, failed logs and client timing traces remain under `build/opcua-integration/`. No failed captures were discarded or relabelled as passing.

Normal integrated Release pair `202796ba7d72282d…` is restored and verified: both cores Ready, ICC connected, saved Wi-Fi association restored, motors disabled, remote permission OFF, GUI/SDRAM checks healthy. Final cold-start confirmation is pending. Physical display/touch, real-driver behavior and powered mechanical commissioning remain separate qualifications.
