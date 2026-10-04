# Step-generator bench test (NUCLEO-H753ZI)

This dedicated image runs the production TIM2/DMA step generator through a
serial menu. It keeps **PE15 / EN_N high (disabled)**. DIR outputs stay low
except during the motor-level `m` case, which restores them afterward. Each
command starts a bounded test after a two-second capture preparation delay. Nothing pulses until you enter a command.

The firmware checks DMA-derived pulse counts, completion/abort states,
monotonic callbacks, stopped GPIO levels, and the autonomous underrun stop.
An independent logic-analyzer or oscilloscope capture is needed to verify the
electrical edges. A firmware `PASS` alone is not proof of the waveform or rotor motion.

The [current hardware measurements](measurements/2026-10-03-step-generator/README.md)
include logic-analyzer and Hantek captures, RTC comparisons, firmware pulse checks
and exported figures for the current crystal-clock configuration. The report
distinguishes physical waveform measurements from firmware-only checks.

## Clock-source configuration

This firmware requires **STLINK-V3 MCO = HSE/5**, a 5 MHz output derived from
ST-Link's 25 MHz crystal. Configure this separately in STLinkUpgrade or
STM32CubeProgrammer's ST-Link firmware/configuration dialog. Read the parameter
back to verify **MCO: HSE/5** before programming the target. The connected
NUCLEO-H753ZI already has this setting, with ST-Link firmware V3J16M9.

For STLinkUpgrade 3.16.9, run from its `AllPlatforms` directory, substituting
your board's serial number. This reapplies the same ST-Link firmware to persist
the clock parameter; keep USB power connected until it reports success:

```sh
java -jar STLinkUpgrade.jar -sn SERIAL -d32_msc -mco_hse 5 -force_prog
java -jar STLinkUpgrade.jar -sn SERIAL -checkParam
```

The matching CubeMX settings are HSE bypass = 5,000,000 Hz and PLL1
M=1, N=192, P=2, Q=24, R=2, input range 4–8 MHz, wide VCO, FRACN=0.
The CPU remains 480 MHz, AHB 240 MHz, APB buses 120 MHz and timer kernels
240 MHz. TIM2's prescaler 23 gives a 10 MHz STEP counter; TIM5's prescaler
239 gives a 1 MHz runtime counter. No software frequency correction is applied.
Recheck ST-Link's MCO after replacing the board or updating its firmware;
CubeMX cannot program that separate setting. See
[ST's MCO guidance](https://community.st.com/stm32-mcus-60/how-to-use-stlink-v3-mco-output-on-nucleo-boards-as-a-precise-clock-source-for-stm32-140173).

## Build and program

Run from the repository root:

```sh
cmake --preset step-test-stm32
cmake --build --preset step-test-stm32
```

This preset uses the optimized Release build with debugger symbols and the
same production HAL, timer clocks and DMA buffers. It produces:

- `build/step-test-stm32/Application.elf` — recommended for OpenOCD/debugging.
- `build/step-test-stm32/Application.hex` — addressed image for a programmer.
- `build/step-test-stm32/Application.bin` — raw image; program at `0x08000000`.

Start with the bare Nucleo, disconnected from the motor-driver/perfboard
harness and external supplies. For this USB-powered setup select **JP2 pins
1–2 (STLK)** and connect the PC to **CN1, the ST-Link USB connector**. Keep
**SB75 ON** for PA0 to CN10.29, **SB51 ON / SB58 OFF** for B1 on PC13, and
the normal 3.3 V MCU supply selection. These are the USB bench settings from
[ST UM2407, table 4, section 7.4.1 and table 14](https://www.st.com/resource/en/user_manual/um2407-stm32h7-nucleo144-boards-mb1364-stmicroelectronics.pdf).
The project's external-powered assembly instead uses JP2 5–6; restore that
selection before reconnecting the documented external supply arrangement.

Program with:

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/step-test-stm32/Application.elf verify reset exit"
```

Open the ST-Link virtual serial port, preferably
`/dev/serial/by-id/*STLINK*if02*`, in your serial terminal at **115200, 8N1,
no flow control**. Commands are a single character followed by Enter;
CR, LF and CRLF line endings are accepted. Local echo is optional.
Press the board's **RESET** button after opening the terminal if you missed
the banner. The expected message is:

```text
[step-test] READY: STEP PA0/PB10/PB11; shared enable PE15 is disabled
```

External flash, SD card, encoder and switches are not required. A
`[storage] unavailable; file access disabled` startup message is expected
with no storage connected. The ordinary runtime still performs its storage
discovery at boot; this image does not require a mounted volume.

## Connect the analyzer

Use 3.3 V compatible digital inputs and a common ground. Connect only the
instrument's inputs and ground, not its supply output.

| Analyzer connection | Nucleo contact | Expected idle level |
| --- | --- | --- |
| Channel 0: M1_STEP | CN10.29, PA0 | Low |
| Channel 1: M2_STEP | CN10.32, PB10 | Low |
| Channel 2: M3_STEP | CN10.34, PB11 | Low |
| Optional channel 3: EN_N | CN10.30, PE15 | High, approximately 3.3 V |
| Ground | CN10.22 | Ground |

Optional DIR probes are CN10.26 / PE12, CN10.10 / PE13, and CN10.8 / PE14;
all stay low except during the motor-level `m` case. Connector numbering is also documented in the
[project pin assignment](../README.md#stepper-gpio-assignment).

Select a rising-edge trigger on M1_STEP, sample at **20 MS/s or faster**, and
capture all three STEP channels simultaneously. Use about 1.2 seconds of
post-trigger recording for cases 2–4, 150 ms for case 5, and 100 ms for cases
1, 6 and 7. The source code's 1 ms start delay is measured from the internal
start call, not from receipt of the serial command or the end of a printed line.

## Run and inspect

Enter `1` through `7` individually first. Arm the capture before sending the
command. `9` runs cases 1–7 in order, stopping on the first failure; use it
after inspecting individual captures. `h` prints the menu.

| Command | M1 / M2 / M3 pulse count | Rising-edge period M1 / M2 / M3 | Expected result |
| --- | --- | --- | --- |
| `1` | 1 / 2 / 3 | 1000 / 1000 / 1000 µs | `Completed`; identifies channels and single-pulse termination |
| `2` | 1000 / 1500 / 2000 | 1000 / 500 / 250 µs | `Completed`; independent rates, counts and finish times |
| `3` | 100000 / 100000 / 100000 | 10 / 10 / 10 µs | `Completed`; 100 kHz on all three axes for about one second |
| `4` | 1024 / 1024 / 1024 | All follow the profile below | `Completed`; changing periods across DMA buffer boundaries |
| `5` | Variable, printed after stop | 100 / 200 / 400 µs | `Stopped`; continuous operation aborted about 100 ms after start |
| `6` | 1000 / 1000 / 1000 | 10 / 20 / 40 µs | `Completed`; refill interrupt deliberately withheld, without a pulse gap |
| `7` | M1: 512; M2/M3: printed counts | 10 / 20 / 40 µs | **`Underrun` is the expected PASS**, with `guard_stopped=1` |
| `8` | 435 / 435 / 435 | 1 / 1 / 1 second | Optional real counter-wrap test, about 7 min 15 s |
| `i` | 100000 / 20000 / first burst + 333 | M1: 10 µs; M2: 40 → 20 µs; M3: 100 then 50 µs | Independent start/stop/restart and live timing change |
| `w` | 64 / 64 / 64 | 100 / 200 / 400 µs | Fast wrap test: idle counter placed 5 ms before overflow |
| `n` | 513 / 777 / 1000, then M1 stopped before its first pulse | 10 / 20 / 40 µs | ISR completion wakeups and interrupt-masked thread wakeup |
| `m` | M2: 800 then ten 10-pulse replacements; M3: timeout-dependent | 62.5 µs | Real `StepperMotor` completion, cancellation and timeout |
| `c` | No pulses | TIM2 counter compared with RTC/LSE | Three 10-second clock measurements, about 31 s total |

For all ordinary completed pulses, high time is **5 µs**, independent of the
period. Axes start through separate calls, so their first rising edges are
staggered by setup time; simultaneous first edges are not an acceptance criterion. Clock tolerance and analyzer sample resolution affect absolute
measurements. Look for extra edges or distinctly extended/missing periods,
especially at DMA boundaries; the waveform must not acquire software-sized gaps.

## Independent clock check (`c`)

Build and flash the same `step-test-stm32` image using the commands above and
the matching 5 MHz clock configuration described above.
Connect the ST-Link USB serial port at **115200 baud, 8N1**, then send **`c` and
Enter**. Wait about **31 seconds** for three result lines followed by `CLOCK VALID`
and `READY for command`. B1 cancels; do not halt the debugger during acquisition.

No measurement instrument or additional wiring is needed. Hantek probes can
remain connected to CH1 / CN10.29 and CH2 / CN10.32 with their common ground;
the logic analyzer can remain disconnected. STEP stays low, DIR stays low and
EN_N stays high throughout this command. The test runs only TIM2's counter,
with all output channels and DMA/interrupt requests disabled.

Each result reports:

- `tim2_ticks`: estimated ticks between RTC second boundaries ten seconds apart;
  nominally **100,000,000**.
- `timer_hz`: counter frequency relative to the RTC's independent 32.768 kHz LSE
  crystal; nominally **10,000,000 Hz**.
- `error_ppm`: positive means TIM2 is fast relative to LSE; **100 ppm = +0.01%**.
  Interpret this estimate together with its sampling bound and LSE tolerance.
- `sampling_bound_ppm`: uncertainty allowance for polling and RTC shadow-register
  synchronization. It does **not** include the LSE crystal's own frequency error.
- `screen`: `WITHIN_0.1_PERCENT`, `FAST`, `SLOW`, or `INCONCLUSIVE`. The 0.1% screen
  is diagnostic, not a final motion accuracy specification. `INCONCLUSIVE` means
  the sampling interval overlaps that threshold.

`CLOCK VALID` means acquisition succeeded; it does not mean the clock met the
timing screen. Green LED means valid acquisition, red means invalid acquisition.
`CLOCK INVALID` gives no frequency conclusion; retain the preceding failure
message. A stopped or incorrectly configured RTC, B1 cancellation, a skipped
RTC second or a sampling gap exceeding a nominal 5 ms invalidates the run.

The reference must be LSE with the existing 127/255 RTC dividers and no active
calibration or time shift. The test brackets each observed RTC second transition
with TIM2 reads, uses three consecutive ten-second windows, and includes a
100 µs synchronization allowance per endpoint. It reads seconds from `RTC_TR`
and unlocks with `RTC_DR`; it does not combine potentially inconsistent subsecond
and calendar snapshots. TIM5/`steady_clock` supplies only an abort timeout,
never the measured elapsed time. RTC date, calibration and clock selection are
left intact. Counter wrap uses the configured `ARR+1 = 0xFFFFFFFF` modulus.

Repeat `c` after a few minutes to check repeatability. Results near zero within
the sampling bounds support agreement between the two board crystals. LSE is
an independent reference, not a calibrated standard; this test cannot establish
absolute frequency to precision finer than its sampling and reference limits.
Keep the complete UART output. Use `3` with the Hantek for a separate waveform
measurement if required; `c` deliberately generates no waveform.

The clock check does not change ST-Link's clock source or apply any correction
factor. Clock-source changes require a separate, matching update of ST-Link
and CubeMX. See [ST's clock-source guidance](https://community.st.com/stm32-mcus-60/how-to-use-stlink-v3-mco-output-on-nucleo-boards-as-a-precise-clock-source-for-stm32-140173)
and [RM0433, RTC calendar reading](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743-753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).

## Pulse profile and stopping checks

Case 4 uses zero-based pulse index `i = 0..1023`:

```text
period[i] = (1000 - min(i, 1023-i)) microseconds
high[i]   = 5 microseconds
```

It accelerates from 1000 µs to 489 µs periods and then decelerates.
`period[i]` is the interval from rising edge `i` to rising edge `i+1`; there
is no subsequent rising edge after the final pulse. Check the transitions near
pulse indices 255/256, 511/512 and 767/768 for continuity.

For cases 1–4 and 6, count **rising edges**, check the last complete 5 µs high
phase, and check that STEP remains low after completion. In case 2, M3 finishes
first, then M2, then M1, without changing the timing of the axes still running.
Keep some post-move recording to spot an unwanted pulse N+1.

Case 5 intentionally aborts at an unsynchronized CPU instant. Compare each
captured rising-edge count with the printed count; do not expect a fixed number
between runs. The last high phase may be shortened. STEP must then stay low,
and counts must stop increasing. Pressing **B1** during a normal finite test
also requests a polled abort, reports the case as failed/cancelled, and returns
to the menu. B1 is a test convenience, not an emergency-stop mechanism. The
IRQ-withholding sections are bounded to at most about 20 ms; case 5 is bounded
to about 100 ms. Serial commands are read only between cases.

Case 6 masks DMA1 stream 0–3 and TIM7 completion-monitor interrupt vectors
for about 4.5 ms, including
the 1 ms start delay. DMA transfers and timer comparisons continue. At least
the M1 transfer-complete flag must be pending when interrupts are restored.
There must be no elongated STEP periods during the delay or when refilling resumes.

Case 7 masks those vectors for about 20 ms, exceeding the queued horizon.
The hardware guard should stop TIM2 after M1's 512th falling edge, approximately
**5.115 ms after M1's first rising edge**. M2/M3 start slightly later, so
compare their captured counts with UART rather than expecting an exact 2:1
ratio. There must be no new rising edges during the remaining interrupt delay,
and restoring interrupts must not restart them.
`guard_stopped=1` means the test observed `TIM2.CEN=0` **before** restoring IRQs
or calling the generator's status/stop methods. An axis can freeze high until
the fault handler runs; a prolonged final high phase is possible on this
intentional fault test.

Case 8 uses the real counter rollover at 429.4967295 seconds after timer
start, without changing its clock or counter registers. Use a long,
transition-compressed capture that still resolves the narrow 5 µs highs;
reducing the sample rate to suit the 1 Hz repetition can miss those pulses.
Check that the 1-second rising-edge spacing remains uninterrupted around
429–430 seconds and that exactly 435 pulses occur on each axis.

Case `i` keeps M1 running at 100 kHz while M2 starts 100 ms later, changes
from 25 kHz to 50 kHz, and completes 20,000 pulses. UART reports the 1-based
first pulse using the new period. Its following rising-edge interval is 20 µs;
all earlier intervals are 40 µs. M3 starts later at 10 kHz, is aborted after
about 100 ms, then restarts with 333 pulses at 20 kHz. Its first burst count is
printed separately; the final M3 count of 333 belongs to the restarted motion.
Check that M1 has no period disturbance at any of these operations. Capture
about 1.2 s after M1's first edge.

Case `w` exercises a real counter overflow in a short acquisition by setting
CNT **only while all axes are idle**. Every axis crosses the wrap with 64 full
pulses and constant spacing. Case 8 additionally exercises long unattended
operation without altering CNT.

Normal axis completion leaves the generator timebase running. The test's
summary aggregates axis states as `Completed`/`Stopped`, then shuts down the
generator before returning to the menu. TIM7 only detects finite completion;
TIM2/DMA generates all pulse edges.

Command `n` waits for each completion event without querying progress, then
checks an API-triggered stop callback under an outer interrupt mask. A higher
priority waiting thread must run only after that mask is released. The final
M1 run count is zero because its second motion is stopped before the first edge;
the earlier 513 pulses remain visible in the capture.

Command `m` runs the actual `StepperMotor` workers. M2 completes 800 pulses,
M3 independently times out, and ten replacement commands cancel their predecessors
and complete 10 pulses each. Worker futures must finish without polling status.
An immediately replaced predecessor can emit pulses if it starts before the
replacement arrives, so the total capture count is not fixed. DIR changes in
this test and returns low afterward. Shared EN_N stays high throughout.

## Interpret the result

A successful case prints, for example:

```text
[step-test] PASS case=2 state=Completed exact=1 counts=1000,1500,2000 callbacks=... guard_stopped=0
```

The yellow LED is on while a test is active. Green means its internal checks
passed; red means they failed. Callback counts vary with serviced interrupts
and are deliberately not equal to the pulse count. After `9`, expect
`SUITE PASS (cases 1..7; wrap is separate)`.

On a failure, save the complete serial output and the three-channel capture.
Do not use debugger halts or breakpoints during a timing capture: they change
the relationship between CPU execution and peripherals. For inspection after
a case, set a breakpoint on `hardware_step_test_complete` and inspect
`hardware_step_test_case`, `hardware_step_test_status`,
`hardware_step_test_pulses`, and `hardware_step_test_guard_stopped`.
Status is `0x600D600D` for PASS or `0xBAD00000 | case` for FAIL.

Run case 2 again after cases 5 and 7 to check clean recovery/restart. Repeat
case 3 several times and compare the counts and spacing across captures.
An oscilloscope can additionally check voltage levels, rise/fall time and
ringing, which digital decoding does not establish.

These tests do not enable motors, test homing/limit-switch policy, validate
encoder feedback, or generate Ethernet/storage traffic. Passing them establishes
the measured unloaded STEP behavior; subsequent tests with the assembled
signal conditioning and concurrent application I/O are still required before
claiming operation at the 100 kHz software ceiling.

To return to the application:

```sh
cmake --preset debug-stm32
cmake --build --preset debug-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "program build/debug-stm32/Application.elf verify reset exit"
```
