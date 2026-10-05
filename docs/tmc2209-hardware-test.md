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
| INDEX | Unconnected |

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
| D4 | UART bus / CN9.4, after the resistor |
| D5 | DIAG / CN9.8 |
| GND | CN10.22 |

D6/D7 are unused. Decode D3/D4 as **115200 baud, 8N1, LSB first**. D3 contains
MCU requests; D4 contains those requests and the driver's replies. D4's local
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
| `check` | UART/configuration suite, always with EN high and STEP stopped. Required before `hold` or `move`. |
| `status` | Print raw GSTAT, DRV_STATUS, IOIN and SG_RESULT without enabling. Useful after a failure. |
| `hold` | After a 2 s preparation delay, enable briefly without STEP, then disable. |
| `move` | After a 2 s delay, enable/settle, emit 400 pulses with DIR low, wait 300 ms, emit 400 with DIR high, then disable. |
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
- A disabled configuration round trip through 32 microsteps / StealthChop /
  interpolation off, then restoration of the motion settings.
- 100 repeated status/configuration checks, including reply CRCs.
- All three controller STEP pads remain low.

Expect `PASS check; EN disabled`, followed by `READY for command`. On failure,
EN stays high; correct wiring/power, optionally read `status`, and rerun `check`.
Do not induce a hardware short circuit or overheat the driver to test DIAG.

Motion settings are **400 mA RMS requested**, approximately **397 mA nominal
quantized current**, with hold equal to run, **16 microsteps**, interpolation
on and **SpreadCycle**. This is a conservative unloaded test setting for both
project motors. UART current control bypasses the potentiometer. The alternate
550 mA / StealthChop configuration is exercised only with outputs disabled.

After `check`, `hold` verifies actual ENN low/high readback without rotating.
Then capture approximately seven seconds around `move`:

- D0 has **two bursts of exactly 400 rising edges**, 800 total.
- Within each burst, the period is **2.5 ms / 400 Hz**, high time **5 µs**.
- D1 is low for the first burst and high for the second, with no pulses at the
  direction transition; both bursts finish with STEP low.
- D2 is low only during the enabled test, returning high at completion.
- D3/D4 show diagnostic reads during motion; pulse timing should remain uniform.
- D5 remains low in a healthy run.

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
