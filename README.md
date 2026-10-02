# NUCLEO-H753ZI C++ template

A CMake-based application template for the STM32 NUCLEO-H753ZI, using ThreadX,
FileX/LevelX, and the STM32H7 HAL. The application prints
`[template] ready` and toggles the green LED every 500 ms.

The CLI, logging system, frontend, pneumo library, and application thread
registry are not dependencies. Platform provenance and intentional differences
are recorded in [platform/runtime/PROVENANCE.md](platform/runtime/PROVENANCE.md).

## Setup and compilers

```sh
git clone --recurse-submodules <this-repository>
# For an existing checkout:
git submodule update --init --recursive
```

Install CMake 3.22 or newer (3.24 or newer for `--fresh`), Ninja, GCC 16,
Arm GCC 16.1 or 16.2 with Newlib, OpenOCD, Arm GDB, Python 3, Git, and `patch`.
The project uses C++26 and the verified GCC 16 libstdc++ integration snapshot.
Unknown compiler majors and unverified Arm libstdc++ snapshots are rejected.
Linux tests download the pinned GoogleTest 1.17.0 source archive.

The VS Code devcontainer uses Arch Linux and checks that both compiler major
versions remain 16. Rebuild it after changing its Dockerfile. It exposes USB
and uses a development user with serial permissions. Host flashing and serial
capture also require access to the ST-Link USB and serial devices.

## Build and run

```sh
cmake --preset debug-stm32
cmake --build --preset debug-stm32
cmake --preset debug-linux
cmake --build --preset debug-linux
./build/debug-linux/Application
```

`release-stm32` and `release-linux` provide the optimized builds. After a
compiler upgrade use `cmake --preset <preset> --fresh` and rebuild cleanly.
Use `-DBUILD_TESTING=OFF` when configuring Linux to build the template without
fetching or building test dependencies.

STM32 builds produce `Application.elf`, `.hex`, `.bin`, and `.map`. Flash with:

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/debug-stm32/Application.elf verify reset exit"
```

The dual-bank target covers all 2 MiB of internal flash. The runtime test image
can exceed the first 1 MiB bank. The ST-Link USB connection also exposes COM1
(USART3) at **115200 baud, 8 data bits, no parity, 1 stop bit**, without flow
control. Prefer its stable `/dev/serial/by-id/*STLINK*if02*` path.

Linux simulates the HAL and uses the ThreadX Linux port. Standard Linux file
I/O uses the host filesystem; explicit FileX tests use simulated media images.
The simulated LED state toggles inside the process; inspect it through the
HAL or Linux debugger. Startup diagnostics and the boot message appear on the terminal.

## Storage and external wiring

Hardware startup requires at least one readable storage volume. Full hardware
conformance requires both the W25Q128 NOR module and an existing FAT SD card;
the reference SD self-test expects a card of at least 8 GiB.

| Device signal | STM32 pin |
| --- | --- |
| W25Q128 CLK | PB2 |
| W25Q128 CS | PG6 |
| W25Q128 IO0 / DI | PD11 |
| W25Q128 IO1 / DO | PD12 |
| W25Q128 IO2 / WP | PE2 |
| W25Q128 IO3 / HOLD | PD13 |
| SD D0, D1, D2, D3 | PC8, PC9, PC10, PC11 |
| SD CLK | PC12 |
| SD CMD | PD2 |
| SD card detect, advisory | PG2 |

Use 3.3 V supplies/signals and a shared ground. The CubeMX configuration is
the pin/clock source of truth. PG2 is D49 on CN8 pin 14; CN8 pin 8 is PC11,
which is already SD D3. SD data/command pins have configured pull-ups.

`/flash` is a 12 MiB FAT volume on the 16 MiB NOR chip, with remaining capacity
reserved for LevelX reclamation. Blank media is formatted on first use; invalid
existing media is not automatically erased. `/sd` mounts the existing FAT
volume and is never automatically formatted on hardware. A 4-bit SD read CRC
failure can trigger the preserved 1-bit retry; diagnostics identify the final
bus width and error. Startup chooses `/flash`, or `/sd` if flash is unavailable,
and panics if neither mounts. The virtual root is read-only; cross-volume
renames return `EXDEV`.

Successful writable-file close and filesystem metadata operations flush the
volume before returning. Explicitly close writers and check errors when
persistence matters. FAT timestamps use UTC and two-second precision.
`RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT` is destructive recovery only; all shipped
presets and the hardware runner set it to `OFF`.

## Extending the application

Keep application code under `src/`. Wrapped startup already initializes the
platform and starts `main` inside ThreadX. Use the board factories, standard
C++ threading/synchronization, and C/C++ streams. Do not initialize ThreadX or
the HAL again from application code. The application entry thread has an 8 KiB
stack; ordinary standard threads default to 4 KiB.

For a thread with a custom priority and stack:

```cpp
#include "runtime/thread.hpp"
auto worker = runtime::thread::create(
    { .name = "Worker", .priority = 17, .stack_size = 12U * 1024U },
    run_worker);
```

Join or cooperatively stop owned threads. `create_jthread` preserves stop-token
injection. A lower ThreadX priority number means higher priority. Applications
may override `hal_panic_handler` with a non-returning handler safe during
startup and interrupt contexts. See the [runtime documentation](platform/runtime/README.md)
for TLS, thread lifecycle, CPU clocks, timestamps, and runtime ownership.

## Validation

```sh
ctest --test-dir build/debug-linux --output-on-failure
ctest --test-dir build/release-linux --output-on-failure
./scripts/run_hardware_self_test.sh
./scripts/run_hardware_self_test.sh --io
./scripts/run_hardware_self_test.sh --configuration Release
./scripts/run_hardware_self_test.sh --io --configuration Release
```

The runtime test preserves all ten reference phases: library surface,
threading/synchronization, atomics/futures/exceptions, TLS/thread exit, libc,
stream/UART stress, clocks, HAL/MAC loopback, NOR storage, and SD storage.
Passing debugger status is `0x600D600D`. UART capture must contain exactly
2048 `A` bytes and 128 `B` bytes between the stress markers.

The separate I/O test checks CR/LF/CRLF console input and echo, then writes and
explicitly closes files, completes rename/delete/timestamp operations on both
volumes, resets the MCU, and verifies data and metadata before cleanup. Its
reserved fixture directories are `/flash/nucleo-io-test.{prepare,committed}`
and `/sd/nucleo-io-test.{prepare,committed}`. It refuses pre-existing fixtures.
After an interrupted/failed run, inspect these directories and remove the test
fixtures before retrying; it does not erase a volume to recover.

Connect the board directly to the Linux Ethernet test interface. To start an
owned peer (requires root to open its raw socket, then drops privileges):

```sh
./scripts/run_hardware_self_test.sh --ethernet --interface enp17s0u1c2
./scripts/run_hardware_self_test.sh --ethernet --interface enp17s0u1c2 --configuration Release
```

Alternatively, when sudo needs an interactive password, start the peer in a
separate terminal and run the test without `--interface`:

```sh
sudo python3 scripts/ethernet_peer.py --interface enp17s0u1c2
./scripts/run_hardware_self_test.sh --ethernet
```

The peer uses the reference raw-frame protocol without IP/interface changes.
The eight Ethernet phases check frame contents/padding, VLANs, filtering,
receive recovery, 1000 round trips, restart, 10/100 Mb/s renegotiation, and
maximum-frame loopback. A 1522-byte board transmission receives a short
validated acknowledgement to accommodate host adapter limits.

The Linux HAL packet-socket test needs `CAP_NET_RAW`; ordinary runs may skip
that case. Run it with the required capability, or in a temporary Linux user
and network namespace with loopback enabled, to obtain complete coverage.

The runner owns and cleans up OpenOCD, detects a busy debugger port, begins
serial capture before resetting firmware, and returns nonzero on failure or
timeout. Stop an active debug session first. It uses only Python's standard
library. Environment overrides are `RUNTIME_TEST_SERIAL_PORT`,
`RUNTIME_TEST_GDB_PORT` (3333), and `RUNTIME_TEST_TIMEOUT_SECONDS` (300).
Logs, compiler versions, UART captures, and `report.json` are retained in
`build/<test-preset>[-release]/validation/`. If an external peer is used, pass
its `--log` argument to retain its counters beside the test report.

Tests replace the application firmware. Afterwards rebuild and flash
`debug-stm32` with the command above to restore the template.

## VS Code and CubeMX

The workspace includes STM32 and Linux debug configurations, build/flash tasks,
and runtime, serial/persistence, Ethernet, and Linux test tasks. Cortex Debug
uses ThreadX awareness and the dual-bank OpenOCD target. Linux GDB passes
ThreadX's SIGUSR1/SIGUSR2 signals without stopping.

CubeMX must retain USER CODE and avoid generating an application `main`.
Configuration-time guards detect lost project hooks, pins, TLS, stack, heap,
and Ethernet buffer layout. Preserve the project linker changes when
regenerating code. Keep application code independent of generated sources.

## Verified migration results

Validated on 2026-10-02 with native GCC 16.2.1, Arm GCC 16.2.0, CMake 4.4.3,
Arm GDB 17.2, and the installed OpenOCD 0.12 development build.

| Check | Debug | Release |
| --- | --- | --- |
| Linux CTest entries | 11/11 | 11/11 |
| Runtime GoogleTest cases | 66/66 | 66/66 |
| HAL cases, including raw socket in a network namespace | 18/18 | 18/18 |
| Board runtime phases and exact UART stress counts | 10/10 | 10/10 |
| Serial input and reset-persistence phases | 3/3 | 3/3 |
| Physical Ethernet phases | 8/8 | 8/8 |

The peer on `enp17s0u1c2` reported no errors. Both storage volumes mounted; the
connected SD card used the preserved 1-bit fallback after a 4-bit CRC error.
A clean export with unmodified pinned dependencies built for both targets
without the reference checkout or test dependencies. Corrupted simulated NOR
and SD contents remained unchanged. The small-volume regression fails against
unpatched FileX and passes with the tracked patch.

The normal Debug template was restored and flash-verified. Hardware LED
transitions were 501 ThreadX ticks apart for a requested 500 ms sleep; Linux
debugger observations were approximately 516 ms apart. The serial boot message
was verified again after debugger detach and reset. OpenOCD enumerated the
application, timer, and reaper threads; this installed OpenOCD build emits an
SP-alias warning when unwinding the suspended timer thread.

Detailed machine-readable results are retained at `build/validation/summary.json`
and in the per-configuration validation directories.
