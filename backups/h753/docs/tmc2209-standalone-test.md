# TMC2209 standalone STEP/DIR test

Build preset: **`tmc-standalone-test-stm32`**. This is a separate image for one
Adafruit #6121 and an unloaded M2/M3 motor, using the existing **M2 signal pins**.
It never instantiates a TMC2209 device or driver UART transport, never reads or
writes driver registers, and skips USART2 initialization even during startup.
PD5/PD6 stay in analog mode with no pull resistors (high impedance).
The ST-Link console still uses the separate **USART3**, at 115200 baud.

## Prepare the hardware

Start with the [single-driver bench wiring](tmc2209-hardware-test.md), with
these changes made **with 24 V and Nucleo USB disconnected**:

| Connection | Standalone setting |
| --- | --- |
| STEP | PB10 / CN10.32, retain 10 kΩ to GND |
| DIR | PE13 / CN10.10 |
| EN | PE15 / CN10.30, retain 10 kΩ to 3.3 V |
| DIAG | PD4 / CN9.8; input only, aborts on a rising edge |
| MS1 and MS2 | Both GND: **1/8 microsteps** after power-up |
| SPRD jumper | Open: default **StealthChop** |
| Adafruit UART / PDN pin | Disconnect from **both PD5 and PD6**, then tie to **VDD / 3.3 V**. This is a static strap that disables automatic standstill current reduction; no UART communication. |
| VDD, GND, 24 V and motor | Retain the bench wiring and confirmed winding pairs |

**Power-cycle the driver, not just the Nucleo.** Remove both its motor supply
and VDD long enough for the rails to discharge. A Nucleo reset or firmware
flash does not clear the driver's previous UART register settings. Our tests
do not program OTP memory; normal volatile settings reset on driver power-up.

**The potentiometer sets motor current.** The UART bench's digital current
setting is not applied. Do not assume the potentiometer's existing position
is suitable, and do not run `hold`/motion until it is set.

For the Adafruit board's 0.05 Ω sense resistors and standard reset settings
(`VSENSE=0`, full standalone current scale), the nominal calculation is:

```text
I_RMS = 0.325 / ((0.05 + 0.02) × sqrt(2)) × VREF / 2.5
      ≈ 1.31 × VREF  [A, with VREF in volts]
```

**VREF around 0.30 V gives approximately 0.39 A RMS**, a conservative initial
standalone test current. Measure VREF at the potentiometer wiper relative to GND with
the driver powered and **EN high**, then adjust the potentiometer. Keep the
probe away from adjacent contacts. This is a conservative initial test point,
not a precision current calibration: the datasheet recommends a higher VREF
range for best analog accuracy. Do not turn the potentiometer up blindly to
compensate for a wiring or motor fault.

## Build and flash

Keep 24 V off while flashing. Restore Nucleo USB power and use:

```sh
cmake --preset tmc-standalone-test-stm32
cmake --build --preset tmc-standalone-test-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/tmc-standalone-test-stm32/Application.elf verify reset exit"
```

Confirm this startup banner before applying 24 V and setting VREF:

```text
[tmc-standalone] READY: STEP/DIR standalone bench; USART2 off; EN disabled; no startup motion
```

Open the console with:

```sh
python3 -m serial.tools.miniterm /dev/serial/by-id/*STLINK*if02* 115200 --eol LF --echo
```

Press Enter for help. Exit miniterm with **Ctrl+]**.

## Commands

| Command | Action |
| --- | --- |
| `check` | With EN high, check STEP-low state, idle timebase, DIAG/B1, USART2 clock disabled and PD5/PD6 high impedance. Required after startup, failure or stop. **No driver communication and no verification of motor current, straps, power or connections.** |
| `hold` | Wait 2 seconds, enable for **5 seconds without pulses**, then disable. |
| `move` | Wait 2 seconds, enable/settle for 1 second, send 400 pulses with DIR low, pause 0.5 seconds, send 400 with DIR high, then disable. |
| `forward` | Same preparation; one 400-pulse leg with DIR low, then disable. |
| `reverse` | Same preparation; one 400-pulse leg with DIR high, then disable. |
| `stop` | Immediately raise EN, wake the worker to stop STEP, and require another `check`. |
| `status` | Report local EN/DIAG levels and bench state. Does not read driver registers. |
| `help` | Show the menu. |

**B1** also disables immediately. DIAG is only an input; its rising edge does
the same and latches a fault. Commands run on a worker so the console remains
responsive. Each operation is bounded and leaves EN high when it finishes or
fails. There is no automatic start, retry, or resume after stopping.

Run **`check` → `hold` → `move`**, waiting for each to finish. With the specified
1/8 straps and a 1.8° motor, each 400-pulse leg is **90° over 2 seconds**, at
7.5 RPM. The driver may make a small alignment jump when enabled; this should
be followed by continuous rotation during a move. `forward` and `reverse`
allow observing one direction at a time; these names specify DIR levels, not
a guaranteed physical clockwise/counterclockwise direction.

No UART-based current limit, configuration verification, or detailed fault
monitoring is available in this image. A `PASS` confirms the bench operation
and commanded pulse count, not actual shaft movement or winding current.

To return to the UART bench image, power down, remove the PDN-to-3.3 V strap,
and restore the shared PD6 RX / 1 kΩ PD5 TX connection before using that image.

## Logic analyzer

The current temporary channel assignment can stay as follows:

| Channel | Signal |
| --- | --- |
| D0 | STEP / PB10 |
| D1 | DIR / PE13 |
| D2 | EN / PE15 |
| D3 and D4 | PD5, now disconnected and high impedance |
| D5 | PD6, now disconnected and high impedance |
| D6 | DIAG |
| D7 | INDEX |
| GND | Common GND |

D3–D5 are unused floating inputs and may show noise; this is not UART traffic
at the driver. To observe the actual PDN pin, move D5 to the Adafruit UART/PDN
pin, which is strapped high. Use only logic-level signals on the analyzer.

Expected STEP period is **5 ms**, high time **5 µs**, with 400 rising edges per
leg and STEP low after stopping. At 1/8 microstepping, INDEX repeats every
**32 STEP pulses**, approximately 160 ms at this speed. Capture about 10 seconds
around `move`. All STEP edges of a completed move occur while EN is low.
`stop`/B1 raises EN immediately; STEP shutdown follows when the worker runs.

References: [Adafruit pinout and standalone controls](https://learn.adafruit.com/adafruit-tmc2209-stepper-motor-driver-breakout-board/pinouts),
[TMC2209 datasheet, configuration pins and current calculation](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf),
[Adafruit schematic download](https://learn.adafruit.com/adafruit-tmc2209-stepper-motor-driver-breakout-board/downloads).
