# Integrated OPC UA frontend

The application exposes `opc.tcp://<GIGA-address>:4840` (Linux: `opc.tcp://127.0.0.1:4840`), namespace **`urn:protomate:control`**, model version 1. It uses anonymous access and SecurityPolicy None on the trusted LAN. Monitoring starts automatically; **remote command permission is OFF after boot**.

**Qualification limit:** sustained Wi-Fi motion currently encounters source-expiry interruptions; see [the measured blocker](verification.md#remaining-blocker-and-bounded-stopping-point). Remote permission remains OFF in the restored normal image. Monitoring and short electrical workflows pass.

The GUI, CLI and OPC UA use one motion client, planner and execution-job history. The server neither implements a second motion engine nor executes CLI text. The standalone [sample](../../examples/opcua_sample/README.md) remains separate and buildable.

- [Generated node and record reference](nodes.md)
- [Frozen machine-readable interface](interface.json)
- [Architecture, ownership and memory](architecture.md)
- [Qualification results and limitations](verification.md)
- [Python client](../../src/ui/opcua/client.py)

## Build and run

Use the existing project toolchain/container and SDK setup:

```sh
cmake --preset debug-linux
cmake --build --preset debug-linux
build/debug-linux/Application

cmake --preset giga-release
cmake --build --preset giga-release
python3 scripts/giga_program.py flash --build build/giga-release
```

The corresponding `release-linux` and `giga-debug` presets are supported. M4 does not link networking or open62541. `giga-motion-api-release` is an explicit motor-disconnected electrical fixture, not production firmware.

## Local network setup

GIGA: open **Settings → Network**, or run:

```sh
python3 scripts/opcua_provision.py
```

Enter credentials at the private local prompt. One versioned profile is stored on internal flash using replacement/backup recovery. It survives application flashing; it is not encrypted storage. Passwords are masked in the GUI and excluded from CLI history, logs and OPC UA. Linux uses the OS network and never provisions host Wi-Fi.

USB CLI:

```text
network status
network scan
network scan-results
network setup
network connect
network disconnect
network forget
opcua status
opcua stop
opcua start
opcua restart
opcua remote on
opcua remote off
```

Setup/connect/scan are asynchronous. `network status` shows the address, state, vendor allocator and separate TX/RX packet pools. Server stop/restart revokes permission and cleans up its remote jobs. Network recovery restores monitoring, not motion ownership. Provisioning never waits for USB during startup.

## Client contract

Install the test/client dependencies into an isolated environment:

```sh
python3 -m venv build/opcua-venv
build/opcua-venv/bin/pip install asyncua pytest pyserial
build/opcua-venv/bin/python src/ui/opcua/client.py opc.tcp://192.168.2.106:4840
```

Use the address reported by your board. The example only reads by default. With calibrated/referenced hardware, locally granted permission and the usual commissioning precautions, `--axis 1 --distance 1` explicitly requests a one-degree relative move. It does not initialize, enable or reference implicitly.

1. Resolve the namespace by URI, not a hardcoded namespace index. Stable NodeIds are strings, such as `ProtoMate/Axes/Axis1/MoveRelative`.
2. Read readiness, configuration identity, quality and freshness. A successful variable read is not proof of valid physical feedback. Check its OPC UA quality and the accompanying validity/provenance fields.
3. Obtain local permission, then call `System/BeginControl` to get a control generation.
4. Send `System/Heartbeat(generation, held_jobs)` every 100 ms. Send periodically without waiting indefinitely for each reply; the example bounds outstanding replies to eight and fails closed if they stall. This does not overcome a real network outage. The eight-entry array contains supervised held-jog job IDs and zero padding. Expiry is 500 ms, checked by the motion-client worker independently of the server and GUI.
5. Submit monotonically increasing request IDs within that generation. Repeating a retained ID returns its original receipt; an expired/unknown old ID is rejected. Do not use a new ID to blindly retry an uncertain command.
6. A `Receipt.submitted` result means local submission, **not IO acceptance or completion**. Follow `System/GetOperation` and the existing job ID through `Jobs/ReadPage` (eight rows maximum).
7. Serialize held-jog submission with heartbeat generation: add the returned job ID before sending the next heartbeat. An omitted held job requests stopping. A terminal job still named by an in-flight heartbeat is harmless and cannot be revived.
8. Release/cancel, session closure, permission revocation or heartbeat expiry cleans up that source's work. Axis/robot work uses controlled stopping; raw motor work retains abort semantics. No reconnect/replay/resume occurs automatically.

Independent local and remote jobs may coexist subject to existing family/axis admission. An active raw-motor queue belongs to one source; another session or local frontend cannot append to it. Explicit Stop/Abort/Disable remain shared system controls, whereas automatic session cleanup affects only that source.

Axis 1/2 coordinates are degrees, Axis 3 is millimetres, raw motors use shaft degrees/rpm, Cartesian destinations use millimetres. Robot dynamics are factors of configured limits. Zero dynamics select existing defaults where documented. Straight-line planning is preview-only; a rejected preview preserves the planner's reason.

Programs, presets, remote filesystem/shell, firmware updates and credential access are not exposed. GUI navigation, camera and numeric-entry widgets remain local. Message acknowledgement does not clear hardware faults.

## Extending the reflected interface

Edit explicit annotations in `src/ui/opcua/catalog/Model.hpp`, bounded input records in `Requests.hpp`, and output records in `Records.hpp`. Bind application behavior through `Call` and `ui::Services`; keep worker ownership unchanged.

`std::meta` discovers annotated members, while `pnm::meta` handles structural/signature/enum metadata. `Schema` checks published identities and supported/bounded types. `Wire` generates separate UA structures, type descriptors, argument metadata and conversions. open62541 encodes the wire; native C++ layouts are never sent.

Published annotation IDs are independent of C++ member spelling. After a deliberate compatible change, regenerate and review both artifacts:

```sh
build/debug-linux/opcua/opcua-model-manifest > docs/opcua/interface.json
python3 src/ui/opcua/catalog/reference.py docs/opcua/interface.json docs/opcua/nodes.md
build/opcua-venv/bin/python -m pytest -q src/ui/opcua/tests/test_schema.py
```

Breaking identity/type changes require a model-version decision, not merely refreshing the golden manifest. No ordinary unannotated member is exported.

## Reproducing verification

```sh
ctest --test-dir build/debug-linux --output-on-failure
build/opcua-venv/bin/python -m pytest -q src/ui/opcua/tests/test_schema.py \
  src/ui/opcua/tests/test_coexistence.py src/ui/opcua/tests/test_motion.py
OPCUA_ENDPOINT=opc.tcp://192.168.2.106:4840 build/opcua-venv/bin/python -m pytest -q -s \
  src/ui/opcua/tests/test_transport.py src/ui/opcua/tests/test_capacity.py \
  src/ui/opcua/tests/test_monitoring.py
```

The motion workflow starts its own native simulator. Do not run another Linux server on port 4840 concurrently. Run GUI latency/screenshot tests without concurrent compiler load.

Electrical tests require the explicit fixture, motor power disconnected, analyzer D0→A7, D1→D3, D2→D2, D3→D15 and common GND:

```sh
build/opcua-venv/bin/python src/ui/opcua/tests/hardware_motion.py \
  --endpoint opc.tcp://192.168.2.106:4840 --case coordinated --output build/my-opcua-capture
```

Cases are `raw`, `coordinated`, `stop`, `loss` and `session --seconds 600`. They retain unfiltered raw captures, hashes, pulse analysis, heartbeat timestamps and CLI diagnostics. They refuse firmware without the electrical-fixture driver marker and restore disabled motors/remote permission OFF in cleanup. Restore normal `giga-release` firmware after fixture testing.
