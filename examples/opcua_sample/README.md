# Standalone Wi-Fi OPC UA sample

This application runs a dummy information model on GIGA M7 through
**open62541 → native NetX event loop → NetX Duo → CYW4343W/SDIO**.
The paired M4 image only sleeps after HAL/runtime startup. It constructs no
motors, motion workers or ICC bridge. The normal application is unchanged.
Linux runs the same model with open62541's POSIX TCP backend.

Anonymous access, SecurityPolicy None: use a trusted local network. There is no
robot control, authentication or encryption in this sample.

## Build and run

```sh
git submodule update --init external/netxduo external/open62541 external/infineon-stm32
python3 -m venv build/opcua-venv
build/opcua-venv/bin/pip install -r examples/opcua_sample/requirements.txt

cmake --preset opcua-sample-linux-debug
cmake --build --preset opcua-sample-linux-debug
ctest --preset opcua-sample-linux-debug --output-on-failure
build/opcua-sample-linux-debug/examples/opcua_sample/opcua-sample
```

Replace `debug` with `release` for the Linux Release build/tests. The optional
executable argument changes the TCP port (default 4840).

```sh
cmake --preset giga-opcua-sample-release
cmake --build --preset giga-opcua-sample-release
python3 scripts/giga_program.py flash --build build/giga-opcua-sample-release
```

`giga-opcua-sample-debug` provides the equivalent Debug pair. Individual core
presets are `giga-io-opcua-{debug,release}` and
`giga-ui-opcua-{debug,release}`. The existing bootstrap, QSPI partitions,
activation manifests and Atmel-ICE workflow are reused. Factory QSPI areas and
the internal filesystem are preserved.

After initial dependency download, incremental builds retain generated adapter
outputs; vendor translation units are not given reflection flags.

USB is not required for startup. Open the GIGA USB CDC console at 115200 or run:

```sh
python3 examples/opcua_sample/provision.py
```

Enter a 2.4 GHz WPA2-Personal SSID and password locally. The password is hidden,
has no command history, and is never written to flash or a host file. The radio
manager and the sample retain credentials in RAM for reconnection; reset or
reflashing clears them. The script's `build/opcua-network-status.json` contains
only the IP address and endpoint. DTR/disconnect discards unfinished input.

| USB command | Function |
|---|---|
| `wifi` | Interactive SSID/password entry, then association and DHCP |
| `scan`, `networks` | Start an asynchronous scan; read at most 16 results and omitted count |
| `status` | Connection, address, server, bounded allocator, packet-pool and worker-stack diagnostics |
| `stop`, `start` | Stop/free or start the OPC UA server; keep Wi-Fi connected |
| `echo` | Replace OPC UA with a temporary TCP echo listener on port 5000 |
| `disconnect` | Stop networking services, then disconnect Wi-Fi |
| `reconnect` | Stop services, rejoin using RAM credentials, reacquire DHCP and restart the selected service |

Only one worker calls the OPC UA server and performs network lifecycle changes.
USB submits bounded requests; radio and NetX callbacks do not call OPC UA.
With no AP or an incorrect password, inspect `status`; initialization and join
failures are reported as SDK result codes. USB remains available while joining.
Radio link loss stops the server; WCM reassociation/DHCP can restore listening.
Existing client sessions are never replayed.

## Information model

Endpoint: `opc.tcp://<GIGA-address>:4840` (Linux: `localhost`). Namespace URI:
`urn:protomate:opcua:sample`. Resolve its index at connection time. String node
identifiers are stable:

| Identifier | Type / behavior |
|---|---|
| `Sample` | Parent object |
| `Uptime` | Read-only Double, seconds since server/model startup |
| `Counter` | Read-only UInt32, increments once per elapsed second; subscribable |
| `Setpoint` | Read/write Double, finite and within −100…100 |
| `ResetCounter` | Method on `Sample`, no arguments |

Invalid writes leave the previous value unchanged. The model uses monotonic
time; GIGA UTC timestamps use the existing RTC. No SNTP is configured, so an
unset RTC is not a claim of correct wall-clock time.

Limits: one session; three secure channels/TCP slots including discovery;
two subscriptions; sixteen monitored items in total; four publish requests;
8 KiB send/receive chunks and 16 KiB messages, at most two chunks. The native
adapter retains at most two unsent buffers per connection. Saturation produces
an explicit error/connection closure, not unbounded buffering. TCP backlog is
bounded separately. This is a server-only IPv4 TCP adapter, not a general
open62541 client, UDP, PubSub or TLS implementation.

## Memory and platform ownership

- A 128 KiB bounded OPC UA allocator uses **80 KiB M7 AXI SRAM + 48 KiB M7
  DTCM**. Its regions never coalesce across the physical gap. No fallback heap.
- The sample worker has a caller-owned **12 KiB DTCM stack**, priority 20.
  The USB console uses the existing runtime workers and main thread.
- NetX packets, SDIO bounce buffers and vendor objects use M7 AXI SRAM, with
  32-byte packet/cache alignment. D-cache remains enabled. Receive DMA uses
  the vendor's bounce buffer; CPU-only UA objects may safely live in DTCM.
- TX/RX packet-pool requests are bounded (12 configured storage slots each;
  report actual usable packet counts from NetX). `low` is the lowest sampled
  TX free count; `empty` reports NetX's actual allocation-exhaustion counter.
  It is not a claim that sampled low-water readings catch every transient.
- General heap statistics include uncommitted linker heap capacity, rather
  than just `mallinfo.fordblks`. `ua_used=0` after stop is the cleanup gate.
- No M4 heap, framebuffer SDRAM or ICC transport capacity is borrowed. The
  common HAL still reserves its existing SRAM4 channel storage, but this
  application does not use it.

SDMMC1 is exclusively M7-owned: PC8–PC11 data, PC12 clock, PD2 command;
PB10 radio enable. In-band SDIO interrupt is used; PI8 is **not** assigned to
EXTI8. Existing motor assignments therefore remain intact. The port validates
the bootstrap's 48 MHz SDMMC kernel clock and never changes the shared PLLs.
The radio is explicitly placed in non-power-saving mode. Country is Germany
(`WHD_COUNTRY_GERMANY`); select the actual deployment country before reuse.

The SDIO peripheral is initialized by the Infineon PAL in
`platform/network/giga/Board.c`, rather than CubeMX's SD-card initialization.
CubeMX regeneration must preserve these reserved radio pins and the existing
48 MHz clock source. No normal-application SD-card backend is enabled.

## Dependencies and maintenance

| Dependency | Pin |
|---|---|
| NetX Duo | `v6.5.0.202601_rel`, `8b6e03ac30ab` |
| open62541 | `v1.5.10`, `93c3bdfc9ea4` |
| Infineon STM32 expansion | 1.8.0, `94101f0710c28b8348f502c7cb04a13d0b70c748` |

Vendor sources remain unmodified. Their own licence files remain in the
submodules. `platform/network` compiles the relevant Infineon WIFI5/PAL/WCM/
NetX integration sources against our existing ThreadX, without Mbed or a second
RTOS. CYW4343W firmware, Murata 1DX CLM and NVRAM come from that pinned package.
The immutable NVRAM is copied at startup and only its MAC address is replaced
with a UID-derived locally administered address; factory flash remains intact.

Two checked build-output adaptations are maintained locally:

1. `src/ui/opcua/util/prepare_netx.py`: supplies a NetX-specific generated config
   header and removes default POSIX event-loop construction. The caller must
   supply the native backend. The unused POSIX address parser is excluded.
2. `platform/network/prepare_sdio.py`: bounds cache invalidation to rounded-up
   receive length and invalidates after DMA without cleaning DMA output back.
   Upstream's `length + 32` could invalidate one line past a full-size buffer.

The WCM 1.8.0 WHD-interface accessor is exported but undeclared in its public
header; its declaration is confined to `Wifi.c` and must be reviewed on upgrade.
Build checks deliberately fail if the expected adaptation points change.

References: [Infineon release](https://github.com/Infineon/AIROC-Wi-Fi-Bluetooth-STM32/blob/master/RELEASE.md),
[open62541 EventLoop API](https://open62541.org/doc/v1.5.0/plugin_eventloop.html).

## Qualification commands

Release GIGA results on 2026-11-11 (pair `30e867c9fa26eaf5`):

| Measurement | Result |
|---|---:|
| Browse/read/write/call/subscribe and TCP pytest workflows | 6 passed |
| Reconnects / server stop-free-start cycles | 20 / 10 |
| Subscription duration / notifications | 600.07 s / 599 |
| Largest notification gap | 1.131 s |
| General heap used / available during endurance | 29,968 / 25,936 bytes |
| UA arena / endurance peak / paused-reader burst peak | 131,072 / 56,840 / 75,304 bytes |
| UA allocations remaining after stop | 0 bytes |
| NetX pools after endurance | Both 11/11 packets available |
| Pool exhaustion / invalid packet releases | 0 / 0 |
| Sample worker stack peak / reserved storage | 4,136 / 12,288 bytes |
| Minimum sampled current-thread stack headroom | 65.8% |
| QSPI M7 / idle M4 image | 879,924 / 112,236 bytes |

Linux Debug and Release each pass twelve GoogleTest cases and the six client
workflows. Both GIGA build configurations pass. Hardware measurements above are
Release, with motor power off and outputs checked inactive. Stack figures come
from ThreadX high-water metadata; they are observed margins, not worst-case
proof. Firmware identities, raw measurements and remaining checks are in
[`validation.json`](validation.json).

```sh
build/opcua-venv/bin/python -m pytest -q \
  examples/opcua_sample/tests/test_server.py examples/opcua_sample/tests/test_tcp.py \
  --endpoint opc.tcp://GIGA_ADDRESS:4840

build/opcua-venv/bin/python examples/opcua_sample/qualify.py \
  --endpoint opc.tcp://GIGA_ADDRESS:4840 --seconds 600 \
  --serial /dev/serial/by-id/YOUR_GIGA_USB_DEVICE

build/opcua-venv/bin/python examples/opcua_sample/hardware_checks.py \
  --serial /dev/serial/by-id/YOUR_GIGA_USB_DEVICE --ssid YOUR_AP_NAME

build/opcua-venv/bin/python examples/opcua_sample/pending_cleanup.py \
  --serial /dev/serial/by-id/YOUR_GIGA_USB_DEVICE \
  --endpoint opc.tcp://GIGA_ADDRESS:4840

# Only after latency/endurance measurements; briefly halts and resumes M7.
python3 examples/opcua_sample/inspect_hardware.py
```

GoogleTest covers bounded allocation and the native adapter's ownership,
backpressure, early disconnect, accept race and deferred-cleanup invariants.
Its NetX seam is explicitly a test double; it does not replace hardware tests.
Pytest exercises actual browse/read/write/call/subscribe, session limits,
twenty reconnects, fragmented handshakes and fragmented application traffic.
The pending-cleanup check pauses a client's receive path, submits a bounded
32-request burst, stops the server with work pending, and verifies zero retained
allocations and a usable restarted server. A send-queue overflow must close the
transport as well as return an error: open62541 marks the channel closing on
that error and relies on the transport's subsequent closure callback.
The endurance script records subscriptions and repeated USB reconnect/status
reads. See `validation.json` for the actual completed checks and pending cases.

`hardware_checks.py` deliberately attempts an absent AP and, when `--ssid` is
provided, one wrong password. It reconnects with the last successful credentials
already held on the GIGA. It never extracts or accepts the real password.
Use the SSID actually shown by `scan`/`networks`: an absent SSID cannot establish
that authentication failure was exercised. The test rejects that false pass.

Physical AP power loss remains a separate manual check: while a client is
subscribed, switch off the test AP, inspect USB diagnostics, then restore the
AP. Verify reassociation/DHCP and reconnect the client explicitly. The automated
disconnect/reconnect cases exercise service cleanup and rejoining but do not
substitute for an unsolicited radio-link interruption.

`verification/` retains the initial endurance data, the reproduced send-overflow
failure and the final checks, with image identity in `validation.json`. Host
GoogleTest uses an explicit NetX test seam; Release hardware is the evidence for
actual SDIO, TCP and ThreadX behavior. Debug firmware is also built, but that
alone does not establish Debug hardware qualification.

After hardware tests, restore the normal firmware:

```sh
cmake --build --preset giga-release
python3 scripts/giga_program.py flash --build build/giga-release
```

Concurrent GUI/motion operation, production remote-control authority and
encrypted/authenticated access remain separate integration work.
