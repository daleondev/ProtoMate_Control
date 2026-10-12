# M1 and M2 bench test

`motor-test-stm32` tests **M1 / DM542T V4.0** and **M2 / Adafruit TMC2209**
together, using the production board factories, timer/DMA STEP generator,
UART driver and feedback providers. M2 uses **StealthChop**, 16 microsteps,
interpolation on, normal INDEX and **550 mA RMS requested run/hold current**
(511 mA nominal after quantization). There is no automatic chopper-mode switch.
M1's microsteps and current remain controlled by its DIP switches.

This image requires no M3, storage, reference switches or robot configuration.
Use secured motors with unloaded shafts: these tests do not home or monitor
travel limits. Leave M3 unpowered. **Enable is shared: even `move m1` energizes
both attached drivers**, although STEP pulses go only to the selected motor.

## Wiring

Use the existing [driver interfaces](../README.md#driver-interface-and-shared-enable)
and connect both fault inputs. Make wiring changes with motor power off.
The existing 24 V motor supply, winding connections and M1 transistor interface
remain in use. Set M1 to **1/16 microstepping / 3200 pulses per revolution**;
retain the corrected winding wiring and appropriate DIP current setting.

| Signal | Nucleo connection | Bench connection |
| --- | --- | --- |
| M1 STEP | PA0 / **CN10.29** | Q1 base through 1 kΩ; collector to DM542T PUL− |
| M1 DIR | PE12 / **CN10.26** | Q2 base through 1 kΩ; collector to DM542T DIR− |
| Shared EN | PE15 / **CN10.30** | Adafruit EN and Q3 base through 1 kΩ; collector to DM542T ENA− |
| M1 ALM | PF2 / **CN9.17** | DM542T ALM+; 4.7 kΩ pull-up to 3.3 V; ALM− to common GND |
| M2 STEP | PB10 / **CN10.32** | Adafruit STEP, with 10 kΩ to GND |
| M2 DIR | PE13 / **CN10.10** | Adafruit DIR |
| M2 UART TX | PD5 / **CN9.6** | 1 kΩ to the UART bus |
| M2 UART RX | PD6 / **CN9.4** | Directly to the UART bus / Adafruit UART |
| M2 DIAG | PD4 / **CN9.8** | Adafruit DIAG |
| M2 INDEX | PD0 / **CN9.25** | Adafruit INDEX; required for `feedback m2` |
| M1 encoder A | PB4 / **CN7.19** | A after the differential receiver and 3.3 V conversion |
| M1 encoder B | PB5 / **CN7.13** | B after the differential receiver and 3.3 V conversion |
| M1 encoder Z | PB6 / **CN12.17** | Z after the differential receiver and 3.3 V conversion; optional |
| 3.3 V | **CN8.7** | Adafruit VDD and logic pull-ups |
| GND | **CN10.22** | Common signal and supply ground |

Use the **470 Ω pull-up to 3.3 V on shared EN**, replacing the single-TMC
test's 10 kΩ pull-up. All three DM542T transistor emitters go to GND; each
base has 10 kΩ to its emitter. PUL+, DIR+ and ENA+ use regulated 5 V; the
DM542T signal-voltage selector stays at **5 V**. Keep ENA wired.

M2 uses **UART address 0**: MS1 and MS2 to GND, SPRD open. Its motor-supply
terminals use 24 V, with the perfboard's local 100 µF / 50 V capacitor.
Its winding pairs remain 1A/1B and 2A/2B.

Power the Nucleo through ST-Link USB, **JP2 pins 1–2 (STLK)**. External 5 V
powers the DM542T signal interface and encoder circuit; do not connect that
rail to the USB-powered Nucleo's 5 V rail. Retain the existing ST-Link
crystal-derived 5 MHz clock configuration.

M1's A/B feedback is optional for basic holding/motion but required for
`feedback m1`. Use the documented [AM26C32 / transistor / HC126 encoder
interface](../README.md#motor-1-encoder); raw encoder outputs are not GPIO
signals. Z edges are displayed but are not required for the A/B test.

## Build and flash

From the repository root:

```sh
cmake --preset motor-test-stm32
cmake --build --preset motor-test-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "gdb_port disabled; tcl_port disabled; telnet_port disabled; program build/motor-test-stm32/Application.elf verify reset exit"
```

Close any other debug session using ST-Link before flashing. Flash with 24 V
off. The console uses **ST-Link USART3, 115200 baud, 8N1**; USART2 is reserved
for the TMC2209. After reset, expect:

```text
[motor-test] READY: M1 DM542T + M2 TMC2209; StealthChop; EN disabled; no startup motion
```

Turn on 24 V after startup, then enter `check`. Startup does not configure or
enable either driver. An ALM fault latched while the motor supply was off is
acknowledged by `check` once both fault inputs are low and B1 is released.

## Commands

Enter commands individually and wait for `READY` before the next operation.
`status`, `stop` and `help` also work during an operation.

| Command | Action |
| --- | --- |
| `check` | Keep drivers disabled, reset the idle STEP generator, configure M2 in StealthChop, verify UART/readback and healthy ALM/DIAG. Required before enabling. |
| `hold` | Enable both, settle for 1 s, hold for 5 s without pulses, then disable. Verify M2 standstill, unchanged phase and no register writes during the hold. |
| `move m1` / `move m2` | 400 pulses at 200 Hz forward, settle, then 400 reverse; disable afterwards. At 1/16 this is 45° out and back, 22.5°/s. |
| `move both` | The same out-and-back test, with both outputs scheduled from the same timer sample. |
| `forward m1 100 100` | One finite leg: motor, pulse count, frequency in Hz. Here M1 moves 11.25° at 11.25°/s. |
| `reverse m2 100 100` | One reverse leg; `both` is also accepted. DIR high is called forward, without assuming a physical clockwise direction. |
| `independent` | M1: 800 pulses at 200 Hz. While that motion continues, M2: 400 forward then 400 reverse at 400 Hz. Confirm M1 continues uninterrupted; return M1 with 800 reverse pulses. |
| `feedback m1` | 400 pulses out and back; require 200 ±4 encoder counts outward, a nonzero sampled velocity, and return within 4 counts. |
| `feedback m2` | Verify INDEX counts and signed velocity at 100/400 Hz, including direction reversals and a short partial-cycle reversal. Undo all commanded excursions. |
| `feedback both` | Run the two feedback tests sequentially. |
| `status` | Live fault levels/latches, enable, busy/qualification, STEP counts, M1 encoder count/velocity/Z edges and M2 INDEX count/velocity. |
| `stop` | Disable both immediately, wake the worker to stop STEP generation, and require a new `check`. B1 and a received Ctrl-C byte also abort. |
| `help` | Print the command summary. |

Optional movement arguments are `[pulses [hz]]`, default **400 / 200**.
Limits are **1–3200 pulses**, **50–1000 Hz**, **20 s maximum per leg**.
Malformed or excessive arguments reject the entire command. Each enabling
command provides a 2 s preparation delay and 1 s enabled settling interval.
Moves use constant velocity, with no acceleration profile; they are bench
checks, not robot trajectories.

Suggested sequence:

```text
check
hold
move m1
move m2
move both
independent
feedback m1
feedback m2
```

Observe that both shafts hold steadily, each selected shaft moves smoothly,
and an unselected shaft holds still. Basic motion checks verify exact
commanded pulse counts and M2's internal electrical phase; **they cannot prove
shaft movement**. M1's dedicated feedback test verifies its physical encoder.
M2 INDEX is electrical feedback, not a rotor encoder, and cannot detect a
stalled or disconnected motor. At 1/16, one INDEX cycle is 64 STEP pulses
(7.2° of commanded motor angle). Counts are not homed robot coordinates.
Disabling the drivers invalidates M2 INDEX phase, so inspect live values
during motion or use the feedback test's result before disable.

ALM/DIAG activation or B1 raises shared EN immediately in the interrupt
callback. The worker then stops both pulse outputs. UART faults, unexpected
STEP states/counts and feedback-test failures also disable both. There is no
automatic retry or resume: correct the cause, release B1 and run `check`.
A DM542T protection fault may require a driver power cycle. After successful
tests, the drivers are disabled and the next command can run without another
`check`. Reset always requires a fresh `check`.

## Logic analyzer

Use the 8-channel analyzer at **24 MHz** on the MCU's 3.3 V signals:

| Channel | Signal |
| --- | --- |
| D0 | M1 STEP / CN10.29 |
| D1 | M2 STEP / CN10.32 |
| D2 | M1 DIR / CN10.26 |
| D3 | M2 DIR / CN10.10 |
| D4 | Shared EN / CN10.30 |
| D5 | M1 ALM / CN9.17 |
| D6 | M2 DIAG / CN9.8 |
| D7 | M2 INDEX / CN9.25 |
| GND | CN10.22 |

For a default `move`, capture about 10 s: each selected STEP output should
have **two bursts of 400 rising edges**, **5 µs high**, **5 ms period**.
Unselected STEP stays low. `move both` schedules corresponding edges at the
same time; `independent` uses 5 ms periods on M1 and 2.5 ms on M2 while M2
finishes and reverses. DIR settles at least 2 ms before each leg. EN is low
only while the drivers are enabled. Healthy ALM and DIAG remain low.

For M1 encoder inspection, D2/D3 can instead capture the **converted 3.3 V**
A/B outputs. The 400-step outward leg should produce 200 quadrature counts,
approximately 50 complete A/B cycles. A/B order reverses on the return.
Z depends on starting angle and is not guaranteed during this 45° excursion.
The debugger result is `hardware_motor_test_status`: `0x600D600D` passes;
`0xBAD00000 | operation` fails. This guide describes expected behavior, not
recorded hardware results.
