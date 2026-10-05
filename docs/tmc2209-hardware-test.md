# Single TMC2209 bench test

For a separate image that **never uses the driver UART**, see the
[standalone STEP/DIR test](tmc2209-standalone-test.md), preset
`tmc-standalone-test-stm32`. Its current setting and power-cycle instructions
differ from this UART-controlled test.

This image tests one Adafruit #6121 with the production USART2 transport,
TMC2209 device driver, board factories and TIM2/DMA STEP generator. It does
not instantiate the robot's `MotionController`; the normal application still
requires both UART drivers before enabling the shared output.

Use either the project's M2 or M3 motor, secured with an unloaded shaft.
Both use the **M2 controller pins** for this test, regardless of which motor
or UART address is selected. No encoder, reference switches, perfboard, buck,
DM542T or second TMC2209 is required. Use the **logic analyzer**, not the Hantek.

## Temporary wiring

Wire with motor power off. Power the Nucleo through **CN1 / ST-Link USB** and
select **JP2 pins 1–2 (STLK)**. Do not attach an external 5 V supply in this
USB-powered setup. Retain the project's crystal-derived ST-Link MCO setting
(HSE/5, 5 MHz). B1 uses PC13, with SB51 ON / SB58 OFF.

| Adafruit connection | Connection |
| --- | --- |
| VDD | Nucleo **CN8 pin 7**, 3.3 V |
| GND | Nucleo **CN10 pin 22**, common with motor-supply negative |
| Motor-supply `+` screw terminal | 24 V positive, initially switched off |
| Motor-supply `−` screw terminal | 24 V negative / common ground |
| STEP | **CN10 pin 32 / PB10**, plus **10 kΩ to GND** |
| DIR | **CN10 pin 10 / PE13** |
| EN | **CN10 pin 30 / PE15**, plus **10 kΩ to 3.3 V** |
| MS1 | GND for initial address 0 |
| MS2 | GND |
| UART | **CN9 pin 4 / PD6** directly, plus **1 kΩ to CN9 pin 6 / PD5** |
| DIAG | **CN9 pin 8 / PD4** |
| INDEX | Logic analyzer D7 (required for `index`); no MCU connection |

Leave **SPRD open**. The UART wire is one node connecting the Adafruit UART
pin, PD6 RX and the far end of the 1 kΩ TX resistor:

```text
CN9.6 / PD5 TX ---- 1 kΩ ----+---- Adafruit UART
CN9.4 / PD6 RX -------------+
```

Connect one winding to **1A/1B** and the other to **2A/2B**. The project's
M2/M3 motor connectors use winding pairs **1–4** and **3–6**; pins 2 and 5
remain unused. Keep the motor supply and winding connections on screw
terminals rather than breadboard contacts. Do not connect/disconnect windings
while powered. Keep supply wires short; a **100 µF capacitor rated at least
35 V** across the driver's supply terminals is useful if available, observing
its polarity. The production perfboard specifies 100 µF / 50 V.

The temporary EN pull-up is **10 kΩ** because only the TMC2209 is connected.
The production perfboard's 470 Ω shared pull-up also drives the DM542T NPN
interface and has a different load.

Sources: [Adafruit pinout](https://learn.adafruit.com/adafruit-tmc2209-stepper-motor-driver-breakout-board/pinouts),
[ST UM2407 connector and power selections](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf),
[project motor drawings](../docs/).

## Logic analyzer

Use the AZDelivery 8-channel analyzer at **24 MHz**, with 3.3 V digital inputs.
Attach only its inputs and GND, not a power-output lead.

| Channel | Signal |
| --- | --- |
| D0 | STEP / CN10.32 |
| D1 | DIR / CN10.10 |
| D2 | EN / CN10.30 |
| D3 | MCU TX / CN9.6, before the 1 kΩ resistor |
| D4 | MCU TX / CN9.6, duplicate of D3 in the current bench wiring |
| D5 | UART bus / CN9.4, after the resistor |
| D6 | DIAG / CN9.8 |
| D7 | Adafruit INDEX |
| GND | CN10.22 |

Decode D3/D5 as **115200 baud, 8N1, LSB first**. D3 contains
MCU requests; D5 contains those requests and the driver's replies. D5's local
echo is expected. Idle is high on UART and EN, low on STEP and DIAG.

## Build and flash

```sh
cmake --preset tmc-test-stm32
cmake --build --preset tmc-test-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/tmc-test-stm32/Application.elf verify reset exit"
```

Flash with 24 V switched off. The artifacts are
`build/tmc-test-stm32/Application.elf`, `.hex` and `.bin`; raw binaries start
at `0x08000000`. The Release preset includes debugger symbols and uses the
production clock configuration. External storage is optional.

The console remains on ST-Link **USART3, 115200 8N1**. Its Linux device is
`/dev/serial/by-id/*STLINK*if02*`. UART driver traffic is on the separate
USART2 pins above. After reset the console reports:

```text
[tmc-test] READY: single TMC2209 bench; address=0; EN disabled; no startup motion
```

No command runs at startup. Confirm the bench banner, then turn on the motor
supply. Commands are complete lines followed by Enter; CR/LF/CRLF work.

## Commands and expected results

| Command | Action |
| --- | --- |
| `mode spread` / `mode stealth` | Select SpreadCycle / StealthChop while idle and disabled. Invalidates the previous check; run `check` to apply and verify. Startup selection is SpreadCycle. |
| `check` | UART/configuration suite, always with EN high and STEP stopped. Required before `hold`, `move` or `index`. |
| `status` | Print selected mode/qualification, raw GSTAT, DRV_STATUS, IOIN and SG_RESULT, reported chopper mode and current scale without enabling. A selected mode is not applied until `check` passes. |
| `hold` | After a 2 s preparation delay, enable/settle for 1 s, hold for 5 s without STEP, then disable. |
| `move` | After a 2 s delay, enable/settle for 1 s, emit 400 pulses with DIR low, wait 300 ms, emit 400 with DIR high, then disable. |
| `index` | StealthChop only. Compare normal INDEX with `index_step`, with interpolation on/off, using external STEP/DIR. Three small out-and-back movements; D7 capture determines the result. |
| `stop` | Immediately raise EN, wake the worker and abort; another `check` is required before motion. |
| `address 0` / `address 1` | Select the physically strapped node; disable and invalidate the previous check. All signal wires stay on M2 pins. |
| `help` | Print the menu. |

Commands execute on a worker so the console can accept `stop` throughout a
test. Other commands are rejected while busy. **B1 immediately raises EN in
its interrupt callback**, then wakes the worker to stop pulse generation.
DIAG does the same and latches a fault. Each test has a bounded duration;
success, failure, cancellation and exception paths leave EN high. Reset also
returns to the disabled, unqualified state.

Run `check` first. It verifies:

- Chip identity, address straps, SPRD state and ENN input.
- Initialization, write acceptance through IFCNT and readable register values.
- Rejection of one intentionally bad-CRC write (`VACTUAL=0`, harmless even if
  unexpectedly accepted): IFCNT must not change.
- Timeout at the absent address, followed by successful communication with the
  selected address.
- A disabled configuration round trip through the opposite chopper mode,
  reduced hold current, 32 microsteps and interpolation off, then restoration
  of the selected comparison settings.
- 100 repeated status/configuration checks, including reply CRCs.
- All three controller STEP pads remain low.

Expect `PASS check; EN disabled`, followed by `READY for command`. On failure,
EN stays high; correct wiring/power, optionally read `status`, and rerun `check`.
Do not induce a hardware short circuit or overheat the driver to test DIAG.

Both chopper modes use **550 mA RMS requested**, approximately **511 mA nominal
quantized current**, with hold equal to run, **16 microsteps**, interpolation
on and identical pulse timing. UART current control bypasses the potentiometer.
The digital current scale is **IRUN=IHOLD=8**, within the datasheet's specified
8–31 range for StealthChop operation. The current readback is the driver's
digital scale, not a measurement of physical winding current. The 1 s enabled
settling interval permits StealthChop's initial standstill auto-tuning and is
the same in both modes.

### Compare holding noise and motion

Enter each command separately and wait for completion before the next:

```text
mode spread
check
hold
mode stealth
check
hold
```

Compare buzzing, fine vibration and holding strength during the five-second
`HOLD` interval. A small initial alignment movement on enabling is distinct
from sustained holding vibration. No pulse is scheduled during either hold.
The test checks ENN, active chopper mode and `CS_ACTUAL=8` after settling,
checks STEP low during the hold, and verifies standstill, unchanged internal
microstep counter (`MSCNT`) and no register writes (`IFCNT`) during the hold.
The internal counter is commanded electrical phase, not a rotor encoder.

If StealthChop is quieter with solid holding, the chopper mode accounts for
the difference. The UART link remains active, performing the same diagnostic
reads in both cases. The test reports electrical/configuration checks as
`PASS`; sound, vibration and shaft behavior still require observation.

Compare motion separately using `mode spread` → `check` → `move`, then
`mode stealth` → `check` → `move`. Mode/address changes while an operation is
busy are rejected; `stop` and B1 remain available. Selecting a mode alone
never enables the motor. Keep the same motor, supply and wiring for both modes.

### Expected logic-analyzer signals

Capture approximately eight seconds around `move` (ten seconds around `hold`):

- D0 has **two bursts of exactly 400 rising edges**, 800 total.
- Within each burst, the period is **2.5 ms / 400 Hz**, high time **5 µs**.
- D1 is low for the first burst and high for the second, with no pulses at the
  direction transition; both bursts finish with STEP low.
- D2 is low only during the enabled test, returning high at completion.
- D3/D5 show diagnostic reads during motion; pulse timing should remain uniform.
- D6 remains low in a healthy run.
- D7 INDEX repeats every 64 STEP pulses at 16 microsteps, about 160 ms at
  this speed. Its phase depends on the internal microstep counter; INDEX
  confirms sequencer activity, not physical rotor motion.

For a 1.8° motor this requests **45° in each direction at 7.5 rpm**. Observe
that the unloaded shaft moves out and back. Firmware counts prove commanded
edges, not rotor position, current accuracy, torque or mechanical performance.
`PASS move` includes exact internal counts for each leg; the analyzer checks
the electrical edges independently.

To check cancellation, start another `move` and send `stop` or press B1 during
the first burst. EN must rise and motion stop; that interrupted case reports
failure/abort rather than pretending all 400 pulses completed. Run `check`
again, then confirm another `move` succeeds. After a fault/abort no motion
resumes automatically.

### Test `index_step` with external STEP/DIR

This test determines whether `GCONF.index_step` is useful as feedback with our
external STEP source. The datasheet describes this signal as toggling on steps
from the **internal** pulse generator; its behavior with our external STEP and
MicroPlyer interpolation must be measured, not assumed.

Retain the current wiring, including **D7 → Adafruit INDEX**, D0 → STEP,
D1 → DIR and D2 → EN. No additional Nucleo connection is required.

```text
mode stealth
check
```

Wait for `check` to pass. Start a **25-second, 24 MHz** logic-analyzer capture,
then enter **`index`** promptly. There is a two-second preparation delay.
The command runs these phases in order, with EN high during reconfiguration
and a separate enabled interval for each phase:

| Phase | `index_step` | Interpolation | Purpose |
| --- | --- | --- | --- |
| 1 | 0 | On | Normal electrical-cycle INDEX; verify the D7 connection |
| 2 | 1 | On | Test step-toggle INDEX with our normal interpolation |
| 3 | 1 | Off | Determine whether interpolation changes the output behavior |

Each phase uses StealthChop, 550 mA requested run/hold current and 16 external
microsteps. After one second of enabled settling, it sends **129 pulses with
DIR low at 100 Hz**, pauses, then **129 pulses with DIR high at 400 Hz**.
Every STEP high time is 5 µs. This commands about **14.51° out and back** on
a 1.8° motor, three times in total. The odd pulse count helps distinguish
output *transitions* from complete high/low cycles. There are **six bursts,
129 rising STEP edges each, 774 total**. The first/second burst in each phase
uses a 10 ms / 2.5 ms STEP period respectively.

Firmware checks configuration readback, exact generated pulse counts and the
`MSCNT` change after each burst. At 16 microsteps, 129 pulses should advance
the electrical counter by **16 modulo 1024** with DIR low and decrease it by
16 with DIR high. It waits 200 ms after each burst before reading that counter
so interpolation can settle, and verifies that each phase returns to its
initial counter. `VACTUAL` is written as zero during every initialization;
this test never starts the internal velocity generator. No driver registers
are written during a burst.

Inspect D7 separately for each phase and direction:

- **Phase 1:** normal INDEX markers, spaced by 64 STEP pulses in one direction.
  Their initial phase can vary. Missing activity here makes a flat trace in
  later phases inconclusive; verify D7/INDEX first.
- **Phases 2/3:** if the pin toggles once per external STEP, expect **129 total
  transitions per burst**, not 129 rising edges. Each STEP rising edge should
  correspond to one INDEX transition, with no unrelated edges while idle.
  A stable toggling output then has half the STEP frequency: 50 Hz / 200 Hz.
- If INDEX stays flat while STEP counts and `MSCNT` checks pass, that mode
  does not expose external steps in the tested configuration.
- If there are multiple transitions per STEP or activity extending into the
  pause, compare phases 2 and 3 for interpolation-related behavior. Record
  the observed ratio and timing rather than treating it as one edge per STEP.

Ignore INDEX transitions caused by changing its function while EN is high;
measure the burst and settling intervals after the one-second enable delay.
Check both output polarities, both directions, and both STEP rates before
deciding that it is suitable for counting feedback.

**`PASS index` means the stimulus, configuration and microstep-counter checks
passed. It does not mean INDEX followed external STEP:** the Nucleo does not
read D7. The captured waveform supplies that answer. It also does not establish
physical rotor position or performance at higher STEP rates.

On completion, stop or failure, firmware disables the driver and attempts to
restore normal INDEX and interpolation on. A restoration failure is reported
and blocks further motion until `check` succeeds. A reset during the test also
requires `check` before motion. `stop` and B1 remain available throughout.

Reference: [TMC2209 datasheet, GCONF on p. 23, MSCNT on p. 31, INDEX on
p. 66 and internal pulse generator on p. 67](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf).

## Check address 1 with the same board

After the driver is disabled, switch off **24 V and USB power**, move **MS1
from GND to 3.3 V**, keep MS2 at GND, and restore USB then motor power. Enter:

```text
address 1
check
hold
move
```

Wait for each command to finish before the next. The driver now represents
the UART address intended for M3, but still uses the M2 test STEP/DIR/DIAG
wiring. One physical board can validate each address separately; it cannot
validate simultaneous two-driver bus operation or the normal controller's
two-driver interlock. Those require the second driver.

For debugging, inspect `hardware_tmc_test_status` and
`hardware_tmc_test_pulses[0..1]` at `hardware_tmc_test_complete` **after** a
case has stopped. `0x600D600D` means pass; `0xBAD00000 | case` means failure.
Do not halt the CPU while the motor is enabled.

### Callback feedback provider test

Also connect **INDEX to PD0 / CN9 pin 25**; leave analyzer D0 on STEP and D1
on INDEX. Run `mode stealth`, `check`, then `feedback`. This exercises the same
`hal::device::IndexFeedback` provider used by the application, with normal INDEX,
1/16 stepping and interpolation enabled. It checks 128 settling pulses, a
256-pulse forward leg at 100 Hz, a 256-pulse return at 400 Hz, and a 16-pulse
out-and-back reversal. Counts, signed velocity and zero velocity at stop are
checked in firmware; STEP remains 5 µs high. All exits disable the motor.
Restore normal firmware using the `debug-stm32` preset after bench testing.
