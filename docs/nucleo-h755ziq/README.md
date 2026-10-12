# H755 application setup

M4 runs `io::Application`; M7 runs `ui::Application`. Each has a complete private
ThreadX/C++ runtime, including threads, synchronization, TLS, allocation,
exceptions, clocks, hardware entropy and FileX/LevelX support. Common HAL device
implementations are compiled for both cores. External motors, Ethernet/SPI
devices and storage media remain unassigned; no storage volume is mounted.

## Ownership

| Resource | M4 | M7 |
| --- | --- | --- |
| Application entry | `src/io/main.cpp` | `src/ui/main.cpp` |
| ThreadX port | `cortex_m4/gnu` | `cortex_m7/gnu` |
| Flash | `0x08100000`, 1 MiB | `0x08000000`, 1 MiB |
| Globals, TLS, heap | D2 `0x30000000`, 280 KiB | AXI `0x24000000`, 512 KiB |
| Interrupt/main stack | `0x30046000`, 8 KiB | DTCM `0x2001E000`, 8 KiB |
| Scheduler/HAL tick | Private SysTick, 1 kHz | Private SysTick, 1 kHz |
| Runtime timer | TIM5, 32-bit, 1 MHz | TIM12, 16-bit, 1 MHz |
| Status LED | LD1 green / PB0 | LD2 yellow / PE1 |
| Additional onboard GPIO | — | LD3 red / PB14, user button / PC13 |
| Console | — | USART3, PD8/PD9, ST-Link VCP, 115200 8N1 |
| RTC and RNG | Serialized access | Initialization and serialized access |
| Inter-core channel | Endpoint M4 | Endpoint M7 |

`hal::core` selects its construction recipes from
[`Catalog.hpp`](../../platform/hal/core/Catalog.hpp) using `util::executing_core()`.
The `[[= Core::M4]]` / `[[= Core::M7]]` entries list available roles and their
board factories; omitted roles return `nullptr`. The HAL uses M4/M7 names;
IO/UI are application roles.

`hal::board` maps hardware and creates devices independently of core ownership.
It exposes all prepared onboard devices with no core parameters or checks.
Driver factories also select hardware independently of the core. Applications
and runtime use `hal::core` to honor the assignments; direct board/driver calls
must respect other users of the same peripheral. GPIO configuration remains
serialized across processors because different pins can share a port register.
Timer wrappers use private handles and route interrupts by peripheral; creation
does not reset or initialize a timer. CubeMX/platform startup retains the
initialization responsibilities shown above. External device wiring is unchanged
and remains unassigned.

M7 configures the direct SMPS supply and shared clocks before releasing M4.
Both CPUs and timer kernels run at 64 MHz from HSI. APB1 is 32 MHz to keep
USART3 within the selected voltage range; its timer kernels remain at 64 MHz.
RTC uses the onboard 32.768 kHz LSE crystal; RNG uses HSI48. RTC/RNG handles
and local locks are private, with hardware semaphores protecting shared access.
The RTC calendar starts at 2000-01-01 if its backup marker is absent and is
preserved across system resets. It is not a synchronized real-world date.
`system_clock` uses it with runtime counter interpolation; `steady_clock`
continues to use the 1 ms scheduler tick. Entropy failure is reported, never
replaced with deterministic numbers.

## Shared-memory transport

```mermaid
flowchart LR
  UI["M7 ui::Application"] <--> UB[UI bus]
  UB <--> UBridge[p2p Bridge]
  UBridge <--> UT[InterCoreTransport]
  UT <--> SRAM["SRAM4 byte queues + HSEM notifications"]
  SRAM <--> IT[InterCoreTransport]
  IT <--> IBridge[p2p Bridge]
  IBridge <--> IB[IO bus]
  IB <--> IO["M4 io::Application"]
```

Both applications require a transport and own a concrete bridge, bus and context.
The bridges register the empty `Topics`, `Services` and `Actions` catalogs in
`src/common/p2p/topics.hpp`, `services.hpp` and `actions.hpp`. These are the
shared definitions to extend with application functionality. Handshake and
heartbeat traffic runs even without application messages. The application
workers sleep until shutdown; there is no LED topic or local blink task.

`application::p2p` remains portable. The separately linked
`application::p2p::intercore` adapter wraps `hal::IInterCoreChannel`.
The channel uses four SPSC queues: normal and urgent in each direction, four
frames per queue, at most 2048 bytes per frame. Queue counters use aligned,
lock-free 32-bit acquire/release accesses; no pointer, mutex, coroutine, or
ThreadX object is shared. Cancellation wakes local coroutine waits, and the
urgent lane has capacity independent of normal traffic.

Both linkers reserve SRAM4 at `0x38000000` (64 KiB); the shared layout occupies
32,880 bytes. Its `NOLOAD` section is outside both startup BSS ranges. M7 zeros
it before releasing M4. M7 configures the region as normal, shareable,
non-cacheable and execute-never before enabling its data cache. Private AXI
RAM stays cached. This follows ST's [dual-core memory guidance](https://www.st.com/resource/en/application_note/an5557-stm32h745755-and-stm32h747757-lines-dualcore-architecture-stmicroelectronics.pdf).

| HSEM | Purpose |
| --- | --- |
| 0 | Boot handshake |
| 1 | Notify M4 of received data, freed space or closure |
| 2 | Notify M7 of received data, freed space or closure |
| 3 | RTC snapshot access |
| 4 | RNG access |
| 5 | GPIO configuration register access |

HSEM interrupts only signal a local runtime notification. A worker wakes
coroutines in thread context. Notifications can coalesce: queue counters are
authoritative, and registration checks existing work. The bridge sends
heartbeats every 100 ms and detects link loss after 500 ms. Link failure or peer
shutdown wakes the application worker, which drains the bridge before stopping
its coroutine context. The main thread then joins both workers.

**Reset both cores together.** A closed channel cannot be reopened during that
boot. Independent core restart/reconnection and cross-session stale-frame
protection remain later work. Neither processor clears a live peer's queues.
A board reset or USB power cycle initializes both endpoints afresh.

## Build

```sh
cmake --preset h755-debug
cmake --build --preset h755-debug
# Optimized pair:
cmake --preset h755-release
cmake --build --preset h755-release
```

Per-core presets remain `h755-io-debug`, `h755-ui-debug` and their Release
variants. Both hardware applications always include P2P and the full runtime.
The separate `h755-led-*` presets retain the bare-metal startup samples.
Both firmware builds report private memory and the same shared reservation;
the two SHARED figures describe one physical region, not two allocations.

CubeMX owns onboard peripheral initialization. The runtime owns scheduler,
fault and syscall hooks. HAL owns dynamic EXTI, timer and HSEM interrupt
registration, shared-memory initialization and MPU setup. The custom linker
owns private memory, compiler TLS and the shared reservation.

## Flash and test

```sh
openocd -f cmake/openocd/nucleo-h755ziq.cfg \
  -c "init; reset halt; targets stm32h7x.cpu0" \
  -c "flash write_image erase build/h755-ui-debug/ui-controller.elf" \
  -c "verify_image build/h755-ui-debug/ui-controller.elf" \
  -c "flash write_image erase build/h755-io-debug/io-controller.elf" \
  -c "verify_image build/h755-io-debug/io-controller.elf" \
  -c "reset run; shutdown"
```

No external wiring is needed. The applications do not drive the LEDs. Reset or
USB reconnection restarts both bridge endpoints. The M7 console reports that
storage is unavailable; that is expected without media.

Each image has six ThreadX threads: timer, main, reaper, p2p context, transport
notification worker and application worker. `hal_last_panic`, CFSR and HFSR
should stay clear. Both scheduler counters and the urgent shared queue counters
should advance as heartbeats are exchanged; normal queues stay idle until
application routes are added. Use each core's own ELF in the debugger. Pausing
a core for more than 500 ms deliberately fails the peer link; reset the board
after such debugging.

Host tests cover serialized delivery, queue saturation, independent urgent
capacity, counter rollover, cancellation, notification races and shutdown.
Application tests cover handshake with empty catalogs, continued heartbeat
traffic while idle, stop during handshake, peer loss and teardown. Lint and
sanitizers are not run automatically for these intermediate migration steps.

Both cores' Debug and Release builds pass without compiler warnings. The Linux
Debug build passes all 34 CTest suites. This application cleanup has not been
flashed or verified on hardware.
