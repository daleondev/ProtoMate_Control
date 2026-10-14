# ThreadX C and C++ runtime port

This directory contains project-owned system runtime integration. It lives
under `platform/`, alongside the HAL, so application code in `src` does not
share a source tree with hardware, toolchain, RTOS, or C library adaptation.

On the H755, each core links its own kernel and runtime instance. The M4 uses
the ThreadX Cortex-M4 port, and M7 uses the Cortex-M7 port. `stm32/` owns the
runtime SysTick integration, per-image TLS/heap linker layout and bounded heap
allocator. See the [H755 setup](../../docs/nucleo-h755ziq/README.md) for memory and
peripheral ownership. `RUNTIME_FILESYSTEM=OFF` omits storage support for this
initial application setup; the threading runtime remains active on both cores.

## Ownership

- `startup.cpp`, `tx_user.h`, and `fx_user.h` own application-thread startup
  and the project ThreadX/FileX configuration. The runtime is ThreadX-specific,
  so nested adapter directories would not add useful abstraction boundaries.
- `libc/` owns Newlib reentrancy and locking, plus the FileX-backed POSIX
  syscall layer. Its `include/` directory contains compatibility headers that
  Newlib does not provide for this target.
- `libstdcxx/` owns the ThreadX gthread/TLS implementation and the libstdc++
  replacement headers. Toolchain-versioned GCC sources live under `vendor/`.
- `tests/` exercises the runtime boundary using the ThreadX Linux port.

The intended dependency direction is `libstdc++ -> libc -> FileX`; ThreadX is
the execution backend shared by both library ports. FileX is not exposed as a
separate project runtime layer.

## Thread lifecycle

Standard threads and blocking synchronization require a running ThreadX
application context. Static constructors can use TLS, exceptions, and local
static initialization before the scheduler starts. Blocking runtime calls
are not supported from interrupt handlers.

`std::this_thread::yield()` suspends the caller for one ThreadX tick (1 ms
at the configured tick rate). This allows lower-priority threads to run:
libstdc++ uses yield while waiting for internal spinlocks, including the
`std::stop_token` callback list. ThreadX's equal-priority relinquish alone
can permanently starve a preempted lock owner. Prefer blocking notifications
or priority-inheriting mutexes in application code; yield is not a precise
timing primitive.

Thread exit completes TLS/TSS destructors before publishing completion to
`join()`. Native `tx_thread_reset()` recreates TLS and Newlib state and clears
thread-specific keys before the thread runs again. Externally terminating a
native ThreadX thread cannot execute its C++ destructors in the target's
context; delete/reset discards those registrations. Standard C++ threads must
exit cooperatively rather than being forcibly terminated through ThreadX.

## File timestamps

The following describes the optional FileX adapter exercised by host tests.

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

The runtime samples each Cortex core's DWT cycle counter at context switches, reads,
and each SysTick. This preserves short execution intervals and extends the
32-bit hardware counter across wraparound. It assumes the configured constant
core frequency and interrupts enabled often enough to sample at least once per
counter revolution (about 67 seconds at the current 64 MHz). Newlib's Arm ABI exposes
`CLOCKS_PER_SEC == 100`, so the public result has 10 ms resolution. Unavailable
or unrepresentable values return `clock_t(-1)` rather than silently wrapping.

## Validation and storage

The H755 bring-up checks are documented in the board setup linked above. The
Linux CTest suite exercises runtime lifecycle, synchronization, TLS, allocation,
filesystem adapters and storage failure/startup policies. Retired board-specific
bench images and their hardware checks are preserved under `backups/`.

FileX/LevelX storage currently runs only in Linux tests with file-backed media.
H755 configuration rejects `RUNTIME_FILESYSTEM=ON` until storage peripherals and
their core ownership are implemented. Its normal firmware uses the unavailable
filesystem adapter instead; console hardware is also not assigned yet.

For explicit host FileX tests, `RUNTIME_STORAGE_REQUIRED=ON` requires at least
one mounted volume. Startup outcomes are cached, including failure; no concurrent
retry/remount API is provided. Normal Linux filesystem calls still use the host
filesystem. The vendor storage libraries and portable file adapters are retained
for later hardware integration.
