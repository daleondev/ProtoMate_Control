# STEP generator measurements

Measurements: **2026-10-03–04**. The current image passes the short pulse-count,
stop-behavior and ±0.1% timing screens on all three physical STEP outputs.
The full rollover capture also has correct counts and stopped outputs; its
strict global-fit residual screen fails because of slow relative clock drift.
The 100 kHz command measures **100.0027 kHz** on the logic analyzer and
**99.9902–99.9903 kHz** on the Hantek. The independent RTC comparison also
passes its ±0.1% screen. These are diagnostic results with the reference
limitations described below.

The controller uses the onboard ST-Link crystal through **MCO HSE/5 = 5 MHz**.
CubeMX configures HSE bypass and PLL1 M=1/N=192/P=2/Q=24/R=2, preserving
480 MHz CPU, 240 MHz timer kernels and a 10 MHz TIM2 STEP counter.
The [ST-Link parameter readback](stlink-parameters.log) confirms HSE/5 and
firmware V3J16M9. No software frequency correction is applied.

## Setup and reproducibility

The NUCLEO-H753ZI runs the production TIM2/DMA STEP engine in the
`step-test-stm32` image. PE15 / EN_N stays high (disabled) and DIR stays low.
Measurements are at the Nucleo pins; no motor-driver interface or rotor is tested.
The [bench procedure](../../stepper-hardware-test.md) defines commands and wiring.

| Instrument input | Signal | Nucleo contact |
| --- | --- | --- |
| Logic D0 / Hantek CH1 | M1 STEP / PA0 | CN10 pin 29 |
| Logic D1 / Hantek CH2 | M2 STEP / PB10 | CN10 pin 32 |
| Logic D2 | M3 STEP / PB11 | CN10 pin 34 |
| Ground | Common ground | CN10 pin 22 |

The instruments are used in separate acquisitions. Both Hantek probes are ×10;
CH1 uses 100 mV/div, CH2 1 V/div, DC coupling. Scope voltage gain and absolute
instrument timebases are uncalibrated. Waveform plots use normalized voltage.
UART uses the ST-Link virtual COM port at 115200 baud, 8N1.

All captures in this directory identify one `Application.bin`, SHA-256:

```text
3a41d6545001abc8447a94a02e6651ca0e23653c128ef48acdbf72f3a7cf3097
```

[Flash verification](firmware-programming.log), [acquisition configuration](acquisition.json),
native sigrok sessions and complete UART logs are retained. The exporter records
SHA-256 hashes of its inputs in `provenance.json`.

## Firmware checks

Cases 1–7 pass on all three axes: finite bursts, independent rates, 100 kHz,
changing profile, abort, delayed DMA interrupts and autonomous underrun stopping.
The separate natural counter-rollover run reports **435/435/435** complete pulses
and `Completed`. These UART results check the HAL's counts and states; independent
captures establish the physical output behavior.

## Logic-analyzer measurements

Cases 1–7 use 24 MS/s, 96,000,000 samples / 4 seconds per capture. All recorded
rising-edge counts match the firmware; rising and falling edges alternate,
each capture starts and ends low, and there are no additional pulses in at least
0.976 seconds of recording after the final falling edge. First rising edges
on all three axes fall in the same 41.667 ns sample. Observed high phases span
5.0000–5.0417 µs, including the final pulse.

| Case | Test | Captured M1 / M2 / M3 pulses | Firmware state |
| --- | --- | --- | --- |
| 1 | Short finite trains | 1 / 2 / 3 | Completed |
| 2 | Independent rates | 1000 / 1500 / 2000 | Completed |
| 3 | 100 kHz on all axes | 100000 / 100000 / 100000 | Completed |
| 4 | Changing period profile | 1024 / 1024 / 1024 | Completed |
| 5 | Abort continuous output | 995 / 498 / 249 | Stopped |
| 6 | Delayed DMA interrupts | 1000 / 1000 / 1000 | Completed |
| 7 | Autonomous underrun stop | 512 / 256 / 128 | Underrun |
| 8 | Natural counter rollover | 435 / 435 / 435 | Completed |

Abort counts depend on the instant of the stop request. Case 7's `Underrun` is
the intended result: the firmware observes `TIM2.CEN=0` while DMA interrupts
are still masked and reports `guard_stopped=1`. The analyzer independently
confirms the pulse counts and stopped outputs; it does not observe the IRQ mask.

| Case / axis | Command | Measured frequency |
| --- | --- | --- |
| 2 / M1 | 1,000 Hz | 1,000.0268 Hz |
| 2 / M2 | 2,000 Hz | 2,000.0536 Hz |
| 2 / M3 | 4,000 Hz | 4,000.1071 Hz |
| 3 / all axes | 100,000 Hz | 100,002.6876 Hz |

The 100 kHz frequency offset is **+0.00269%** relative to the analyzer.
Every short capture passes both the ±0.1% nominal-period screen and the
two-sample residual screen after fitting one mean clock ratio. The largest
short-capture interval residual is under 43 ns (screen: 83.33 ns). This fit is
only an analysis check; the exported traces retain their measured time scale.
The changing profile crosses DMA buffer boundaries correctly. Case 6 continues
through a buffer boundary while its refill interrupt is withheld, without a
software-sized gap in the pulse train.

### Full counter rollover

Case 8 records 440 seconds at 4 MS/s (1,760,000,000 samples). All three outputs
contain **435 rising and 435 falling edges**, with 5.00–5.25 µs observed high
phases and **3.9818 seconds** of quiet recording after the final falling edge.
Corresponding edges on the three channels differ by at most one analyzer
sample (0.25 µs) throughout the run.
There are no extra or missing pulses across the timer's natural rollover.

One-second periods range from **999,970.50 to 999,972.50 µs**, with a mean of
999,971.2483 µs, about +0.00288% frequency offset relative to the analyzer.
This passes the ±0.1% nominal-frequency screen. The periods drift gradually
over the recording: the first 50 average 999,972.135 µs and the final 34
average 999,970.6912 µs. Adjacent periods differ by at most **0.25 µs**, one
analyzer sample. The rollover interval (zero-based index 429) is
999,970.50 µs, only 0.20 µs below the preceding 20-interval mean.

The original two-sample residual screen fits one constant clock ratio over the
whole recording. It allows 0.50 µs at 4 MS/s; the observed maximum residual is
**1.252 µs**, so `case-8.passed` remains **false** in the exported JSON.
This is a documented timing-screen failure, despite correct counts and firmware
`PASS`. The gradual variation is consistent with drift between the controller
and analyzer clocks. These uncalibrated references cannot identify its source;
there is no resolved sudden disturbance at rollover. No samples or failing
results are discarded. See [the rollover figure](08-counter-wrap.png) and
[computed interval statistics](wrap-analysis.json).

## Oscilloscope timing

| Acquisition | M1 frequency | M2 frequency | Rising edges per analyzed channel |
| --- | --- | --- | --- |
| 8 MS/s | 99,990.248 Hz | 99,990.285 Hz | 13,006 |
| 16 MS/s | 99,990.239 Hz | 99,990.222 Hz | 6,453 |

The command is 100,000 Hz on all three axes. The Hantek observes approximately
**−0.0098%** frequency offset on M1 and M2. Each capture contains 1,048,576 samples
per channel: 131.072 ms at 8 MS/s or 65.536 ms at 16 MS/s. These are interior
segments of finite bursts; they do not independently prove full burst counts.

The first 1 ms is excluded by a fixed acquisition-settling rule. All remaining
intervals are included; none is removed as an outlier. Threshold crossings are
linearly interpolated between adjacent samples. Changing thresholds from 30%
to 70% of signal swing changes measured frequency by less than 0.041 Hz.
The two simultaneous channels agree within 0.037 Hz. No analyzed sample clips.
Individual observed periods span 9.9979–10.0171 µs across both channels and rates;
this includes sampling and analog threshold effects.

## Independent RTC comparison

Command `c` measures TIM2 against the independent 32.768 kHz LSE crystal, using
three consecutive ten-second windows per run, with STEP held low. Two runs,
separated by the pulse suite and long rollover test, give six estimates from
**−0.020% to 0.000%**, each with a **±0.022%** sampling allowance. Every window
passes the ±0.1% diagnostic screen. RTC calibration
is zero and its dividers are 127/255. Error bars include the polling and
synchronization allowance; LSE crystal tolerance is additional. Near-zero point
estimates do not establish exact frequency or sub-ppm precision.

## Interpretation and limits

The diagnostic frequency screen is ±0.1%, not a final robot accuracy specification.
The remaining approximately 0.01% Hantek offset cannot be assigned to the controller
or instrument without a calibrated reference. Applying a software correction from
these readings would not be justified.

The analyzer and Hantek differ by approximately 0.0124% at 100 kHz. Their
individual timebases and the board crystals are not calibrated; the RTC polling
bound is wider than this difference. These measurements support using the
crystal configuration without establishing absolute accuracy at the ppm level.
The long-run drift is about 2 ppm peak to peak. Further clock trimming or
changes to the STEP scheduler are not justified by these measurements;
isolating that drift requires a more stable, calibrated reference.

Electrical validation at the STEP pins does not prove torque, encoder position,
transistor/driver timing or motion under load. Ethernet/storage traffic and the
assembled motor interfaces require their own integration tests.

## Exports

Run from the repository root with Python 3, NumPy and Matplotlib installed:

```sh
python3 docs/measurements/2026-10-03-step-generator/export_results.py
```

The script reads saved measurements without accessing hardware, exports PNG figures
and [the 11-page PDF](step-generator-measurements.pdf), and writes CSV/JSON summaries.
Firmware and production Release builds pass; the 31 host model/register tests pass.
The verified test image remains on the Nucleo, idle at its UART menu with
driver enable inactive.
