# ThreadX C and C++ runtime port

This directory contains project-owned system runtime integration. It lives
under `platform/`, alongside the HAL, so application code in `src` does not
share a source tree with hardware, toolchain, RTOS, or C library adaptation.

## Ownership

- `startup.cpp`, `tx_user.h`, and `fx_user.h` own application-thread startup
  and the project ThreadX/FileX configuration. The runtime is ThreadX-specific,
  so nested adapter directories would not add useful abstraction boundaries.
- `libc/` owns Newlib reentrancy and locking, plus the FileX-backed POSIX
  syscall layer. Its `include/` directory contains compatibility headers that
  Newlib does not provide for this target.
- `libstdcxx/` owns the ThreadX gthread/TLS implementation and the libstdc++
  replacement headers. Toolchain-versioned GCC sources live under `vendor/`.
- `tests/` exercises the complete runtime boundary in Linux simulation and in
  the board-resident self-test image.

The intended dependency direction is `libstdc++ -> libc -> FileX`; ThreadX is
the execution backend shared by both library ports. FileX is not exposed as a
separate project runtime layer.

## Thread lifecycle

Standard threads and blocking synchronization require a running ThreadX
application context. Static constructors can use TLS, exceptions, and local
static initialization before the scheduler starts. Blocking runtime calls
are not supported from interrupt handlers.

Thread exit completes TLS/TSS destructors before publishing completion to
`join()`. Native `tx_thread_reset()` recreates TLS and Newlib state and clears
thread-specific keys before the thread runs again. Externally terminating a
native ThreadX thread cannot execute its C++ destructors in the target's
context; delete/reset discards those registrations. Standard C++ threads must
exit cooperatively rather than being forcibly terminated through ThreadX.

## File timestamps

`std::filesystem::last_write_time(path, time)` supports files and directories
on `/flash` and `/sd`, through the FileX-backed `utime()` adapter. Timestamps
use UTC, as the existing timestamp reader does. FAT stores dates from 1980
through 2107 with two-second precision; fractional/odd seconds round down.
Out-of-range dates report `value_too_large`. Virtual and volume roots have no
writable directory entry and report `read_only_file_system`.

Setting a timestamp flushes pending file writes before applying the new date,
then flushes the metadata. Closing an already-open writer without another
write preserves the requested date. Access time is not stored independently
by this adapter.

## CPU time

On STM32, `std::clock()` reports accumulated execution time across all ThreadX
threads since scheduler startup, excluding scheduler idle intervals. Interrupt
overhead while a thread is scheduled is included; interrupts serviced entirely
while idle are not charged to a thread. It is not a per-thread clock or a full
interrupt-load monitor. Linux continues to use its native process CPU clock.

The runtime samples the Cortex-M7 DWT cycle counter at context switches, reads,
and each SysTick. This preserves short execution intervals and extends the
32-bit hardware counter across wraparound. It assumes the configured constant
core frequency and interrupts enabled often enough to sample at least once per
counter revolution (about 8.95 seconds at 480 MHz). Newlib's Arm ABI exposes
`CLOCKS_PER_SEC == 100`, so the public result has 10 ms resolution. Unavailable
or unrepresentable values return `clock_t(-1)` rather than silently wrapping.

## Hardware validation

`scripts/run_hardware_self_test.sh` exercises the runtime and storage on the
Nucleo. Its HAL phase includes the onboard LED/button GPIOs and internal
Ethernet MAC loopback, including descriptor reuse, maximum double-tagged frames,
filtering with small receive buffers, queued-packet restart, and transmit
timeout/retry/cancellation checks. Ethernet loopback does not require a network
cable. For cable traffic and 10/100 Mb/s renegotiation, use the separate
`scripts/run_hardware_self_test.sh --ethernet` flow described in the project README.

The console stress phase preempts a long UART write with another writer.
A capture at 115200 baud must contain exactly 2048 `A` bytes and 128 `B` bytes
between `[uart-stress-begin]` and `[uart-stress-end]`. The debugger result
checks write return values; the UART capture also verifies actual delivery.

## Template validation additions

The hardware runner verifies the UART stress capture automatically and retains
GDB, OpenOCD, UART, compiler, and JSON results under the validation build directory.
`--io` selects a separate test image that verifies CR/LF/CRLF input and checks
closed data and completed metadata operations after a debugger-controlled MCU
reset on both volumes. It uses reserved test directories and cleans up after
success. See the project README for recovery after an interrupted run.

[PROVENANCE.md](PROVENANCE.md) records imported dependency revisions and every
intentional platform difference, including the build-directory FileX patch.


## Optional persistent storage

By default, no mounted FileX volumes produce one console warning and application
startup continues. File operations fail with `ENODEV`; console descriptors
remain usable. `RUNTIME_STORAGE_REQUIRED=ON` restores a deliberate startup
panic when neither volume mounts. One mounted volume satisfies this setting.
Existing runtime and persistence conformance presets select required storage.

`runtime_filex_initialize()` retains its void C ABI. Its completed outcome is
cached, including unavailable storage; `runtime::filex::initialized()` is true
only when file access is available. `runtime::storage::initialize()` also caches
its result and retains diagnostics. Initialization is performed on the startup
thread before application entry; no concurrent retry/remount API is provided.
New media require reset. Missing and corrupt media follow the same policy, with
existing blank-NOR initialization and no automatic physical SD formatting.

The default policy intentionally differs from the reference's required-storage
startup. Linux host filesystem behavior is unchanged. The dedicated startup
probes validate explicit FileX adapters on Linux and Newlib/std-library file
integration on STM32; physical and injected hardware evidence are reported
separately. See the root README for build options, test commands, and restoration.

The pinned STM32 HAL SD driver receives a reviewed build-copy patch adding
a five-second elapsed-time check to operating-voltage negotiation.
The original trial limit and four-bit/one-bit fallback remain active. Without
this elapsed-time bound, an absent card can spend about 76 seconds negotiating
across both attempts. The patch is in
`platform/runtime/libc/patches/stm32-sd-power-on-deadline.patch`; CMake verifies
both original and patched checksums and leaves the vendor submodule unchanged.
This is an additional intentional difference from the reference runtime.

[Storage validation results](STORAGE_VALIDATION.md) record passing checks and the physical
configurations that remain untested.
