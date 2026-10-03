# STEP generator measurements

The TIM2/DMA STEP generator produces the requested finite pulse
counts on all three outputs. Independently captured counts also match the
firmware's counts for software abort and autonomous underrun stop.
**Both instruments measure a fast STEP rate:** a requested 100.000 kHz
measures **100.643 kHz on the logic analyzer** and **100.543–100.639 kHz on
the Hantek oscilloscope**. Simultaneously measured M1 and M2 agree within
0.02 Hz in each retained scope capture. The independent on-board RTC comparison
measures **TIM2 0.58–0.66% fast relative to LSE**, with a polling/synchronization
allowance of **±0.022%**. This establishes a main-clock/reference discrepancy
without either external instrument. Absolute calibration and the precise
clock-source diagnosis remain open; overall timing conformance is not established.

Measurements: 2026-10-03. The test firmware remains programmed on the Nucleo;
its shared driver enable is inactive and it waits for a serial command.

## Setup and firmware

| Item | Configuration |
| --- | --- |
| Controller | NUCLEO-H753ZI, production TIM2/DMA STEP generator |
| Test image | `step-test-stm32`, optimized build with debugger symbols; OpenOCD programming verified |
| Logic analyzer | AZDelivery 8-channel logic analyzer, `fx2lafw`, `sigrok-cli 0.7.2` |
| Oscilloscope | Hantek 6022BE, `hantek-6xxx`, `sigrok-cli 0.7.2`; two channels sampled together |
| Scope captures | 8 and 16 MS/s, 1,048,576 samples per channel per capture, DC coupling |
| Scope probes / ranges | Both probes set to ×10; CH1 100 mV/div and CH2 1 V/div at the instrument inputs |
| Internal clock comparison | RTC/LSE 32.768 kHz crystal; two runs of three consecutive 10-second windows; STEP held low |
| Cases 1–7 | 24 MS/s, 41.667 ns sample spacing, 96,000,000 samples / 4 s per capture |
| Case 8 | 4 MS/s, 250 ns sample spacing, 1,760,000,000 samples / 440 s; resolves each nominal 5 µs high phase |
| Serial | ST-Link virtual COM port, 115200 baud, 8N1 |
| Driver controls | Firmware holds PE15 / EN_N high and DIR outputs low |
| Timer | Nominal 240 MHz input / 24 = 10 MHz counter, `ARR = 0xFFFFFFFE` |

Pulse-capture `Application.bin` SHA-256:

```text
b57c9aa969940a73cbe1300cfb11463e8fcdd6f228e4b7e2c7dc35bd99ce2c6f
```

The [pulse-capture programming log](firmware-programming.log) records successful
flash verification. The RTC comparison and UART suite use the image containing
the `c` diagnostic, with the same main-clock and STEP-engine configuration.
This image remains programmed on the Nucleo; its `Application.bin` SHA-256 is:

```text
e970fa9662bc0f75e0a1a5091dbc4dcef196cb20bab49e52a58a178dc6abff14
```

Its [programming log](clock-reference-programming.log) also records successful
flash verification. Image identities are preserved per measurement; the RTC
test does not change the configured 8 MHz HSE assumption or ST-Link's output.

The [bench source](../../../platform/hal/tests/hardware_step_test.cpp) and
[manual procedure](../../stepper-hardware-test.md) define the test cases.

| Analyzer input | Signal | Nucleo contact |
| --- | --- | --- |
| D0 | M1 STEP, PA0 | CN10 pin 29 |
| D1 | M2 STEP, PB10 | CN10 pin 32 |
| D2 | M3 STEP, PB11 | CN10 pin 34 |
| Hantek CH1 | M1 STEP, PA0 | CN10 pin 29 |
| Hantek CH2 | M2 STEP, PB10 | CN10 pin 32 |
| Ground | GND | CN10 pin 22 |

Case 1's distinct 1/2/3 counts confirm the STEP channel mapping. D3 is high
throughout the captures, but its physical connection to the optional EN_N
probe is not independently established. Driver-control state is checked by
the firmware. Analog scope traces are retained, but their gain and offset
are not calibrated; the report uses them for timing rather than an absolute
voltage or overshoot specification.

## Pulse counts and stopping

| Case | Measurement | Captured M1 / M2 / M3 | Firmware state | Count comparison |
| --- | --- | --- | --- | --- |
| 1 | Short finite trains | 1 / 2 / 3 | `Completed` | Match |
| 2 | Independent rates and finish times | 1000 / 1500 / 2000 | `Completed` | Match |
| 3 | Nominal 100 kHz, three axes | 100000 / 100000 / 100000 | `Completed` | Match |
| 4 | Changing-period profile | 1024 / 1024 / 1024 | `Completed` | Match |
| 5 | Continuous trains, software abort | 995 / 498 / 249 | `Stopped` | Match |
| 6 | DMA interrupts withheld for about 4.5 ms | 1000 / 1000 / 1000 | `Completed` | Match |
| 7 | DMA interrupts withheld for about 20 ms | 512 / 256 / 128 | `Underrun` | Match |
| 8 | Real 32-bit counter rollover | 435 / 435 / 435 | `Completed` | Match |

Each listed case reports firmware `PASS` and `exact=1`. Case 5's counts depend
on the abort instant; they are the measured counts for this capture, not fixed
targets. Case 7's `Underrun` is the intended test outcome.

The short captures contain equal numbers of rising and falling edges, start
and end low, and show no additional pulses during at least 0.995 s of quiet
recording following the final pulse. The three first rising edges fall in
the same analyzer sample. Measured high phases range from 4.958 to 5.000 µs
at 24 MS/s.

Case 2 shows independent completion: M3 stops first, then M2, then M1. Case 4
follows the changing-period profile across the DMA buffers. Case 6 continues
through the first buffer boundary with its refill interrupt withheld; M1's
observed periods span 9.875–9.958 µs, with no interrupt-sized gap.

In case 7 the last M1 falling edge occurs 5.083125 ms from the first rising
edge. All outputs then remain low. The firmware reports `guard_stopped=1`,
meaning it observes `TIM2.CEN=0` while the DMA interrupt vectors are still
masked, before its status/stop calls. Together, this readback and the
independent capture support autonomous guard stopping. The analyzer does
not directly observe the interrupt mask or the timer register.

Case 8 spans the real counter wrap without forcing the counter or changing
its clock. All three channels contain 435 complete pulses and remain low for
the final 6.688 s of the 440 s recording. There is no missing pulse or extra
period at rollover. Across the recording, the nominal one-second periods
range from 992.634250 to 994.670750 ms, with a mean of 993.765213 ms. High
phases occupy 19 or 20 samples at 4 MS/s, corresponding to 4.75 or 5.00 µs.
The rollover interval is identified from the scheduled timer ticks, between
zero-based pulse indices 429 and 430; the counter itself is not probed.
That interval measures 993.678250 ms, within the surrounding period range.

## Timing accuracy

| Case / axis | Programmed period | Measured mean period | Measured frequency |
| --- | --- | --- | --- |
| 2 / M1 | 1000 µs | 993.533784 µs | 1006.508 Hz |
| 2 / M2 | 500 µs | 496.716811 µs | 2013.220 Hz |
| 2 / M3 | 250 µs | 248.349904 µs | 4026.577 Hz |
| 3 / all axes | 10 µs | 9.936129 µs | 100642.816 Hz |

Case 3's individual periods span 9.916667–9.958333 µs. In case 2, M1's
periods span 992.458333–994.416667 µs, so the timing difference includes
variation as well as a mean offset. The figures retain these measured times
without rescaling the traces to the requested frequency.

The analysis screens nominal period error against 0.1% and interval residuals
against two sample periods, using a fitted mean clock ratio. These are
diagnostic thresholds, not an agreed robot accuracy specification. The
nominal-period screen fails, and several captures also exceed the residual
screen. Accordingly, `passed: false` in the measurement JSON is consistent
with firmware `PASS` and matching physical pulse counts.

### Independent oscilloscope measurements

The Hantek captures M1 and M2 during case 3, using the same programmed image.
The logic analyzer is disconnected for these measurements. The scope captures
at different sample rates are also separate bursts, so differences between
their mean frequencies include any change in the board clock between bursts.

| Hantek sample rate | CH1 / M1 frequency | CH2 / M2 frequency | Rising edges per channel in analyzed segment |
| --- | --- | --- | --- |
| 8 MS/s | 100543.470 Hz | 100543.486 Hz | 13078 |
| 16 MS/s | 100638.595 Hz | 100638.581 Hz | 6494 |

Each capture contains 1,048,576 samples per channel: 131.072 ms at 8 MS/s
or 65.536 ms at 16 MS/s. The first 1 ms is excluded from frequency analysis
to avoid acquisition-start settling artifacts; the complete unmodified
capture is retained. Every remaining interval is included. No period is
removed as an outlier. All analyzed rising-edge periods are between 9.5 and
10.5 µs, and both channels have the same edge count in each segment. Neither
channel reaches the ADC rails in the analyzed window; CH2 reaches a rail only
within the excluded startup region.

Rising times use linear interpolation between adjacent ADC samples at the
midpoint of the measured low/high levels. Frequency is `(edge count - 1)`
divided by elapsed time between the first and last analyzed rising edges.
Thresholds at 30%, 50% and 70% of the measured swing give the same edge count;
their frequency spread is recorded in [scope-results.json](scope-results.json).

Both scope channels agree on a positive frequency offset, at both retained
sample rates. This is independent support for the fast-rate finding; it is
not evidence of a disagreement between M1 and M2. It also does not prove
which oscillator is inaccurate without a calibrated reference.

The scope records interior segments, not the entire 100,000-pulse burst.
UART reports `PASS`, `Completed`, `exact=1`, and 100000/100000/100000 for
each run; complete physical pulse counting is provided by the logic-analyzer
captures. The scope's voltage scale is not used to claim that the GPIO exceeds
its supply voltage. CH2 uses the wider input range to avoid clipping.

For this driver/setup, timing analysis uses 8 or 16 MS/s and a settled
acquisition window. Higher-rate acquisition is not accepted for timing
validation because its traces contain discontinuities. The upstream
[Hantek driver](https://github.com/sigrokproject/libsigrok/blob/master/src/hardware/hantek-6xxx/api.c)
transports both physical channels even when only one is exported; USB
throughput is a plausible contributor, not a conclusively established cause.

The firmware assumes an 8 MHz HSE bypass clock from ST-Link. ST documents
an HSI/2 8 MHz STLINK-V3 MCO option and crystal-derived MCO alternatives.
This makes the clock source a plausible contributor to the observed error;
its selection and output frequency have not been directly checked. The
MCU clock and both instrument timebases remain uncalibrated. See
[ST's MCO clock-source guidance](https://community.st.com/stm32-mcus-60/how-to-use-stlink-v3-mco-output-on-nucleo-boards-as-a-precise-clock-source-for-stm32-140173).

The outstanding timing work is to verify the clock against a suitable
reference, establish the required accuracy, and coordinate any clock-source
selection with the CubeMX HSE/PLL configuration before remeasurement.

### Independent on-board RTC comparison

UART command `c` runs TIM2's counter while keeping STEP and DIR low and EN_N
high. It measures timer ticks between RTC second transitions, without using
the Hantek or logic analyzer. The RTC uses the separate 32.768 kHz LSE crystal
with asynchronous/synchronous dividers 127/255 and no calibration adjustment.
Read-back configuration is `BDCR=00008103`, `RTC_CR=00000000`, `PRER=007f00ff`,
`ISR=00000027`, `CALR=00000000`. The cleared `INITS` bit reflects the default
calendar year 00; advancing RTC seconds supply the reference.

| Run / window | Timer frequency relative to RTC | Offset | Sampling allowance |
| --- | --- | --- | --- |
| 1 / 1 | 10,060,000.1 Hz | +0.600% | ±0.022% |
| 1 / 2 | 10,058,000.1 Hz | +0.580% | ±0.022% |
| 1 / 3 | 10,059,999.9 Hz | +0.600% | ±0.022% |
| 2 / 1 | 10,065,999.9 Hz | +0.660% | ±0.022% |
| 2 / 2 | 10,064,000.1 Hz | +0.640% | ±0.022% |
| 2 / 3 | 10,064,000.0 Hz | +0.640% | ±0.022% |

The nominal counter frequency is 10,000,000 Hz. Every window is classified
`FAST`: even allowing for polling and synchronization, its offset exceeds the
diagnostic +0.1% screen. `CLOCK VALID` confirms acquisition, not timing accuracy.
The displayed decimal places are calculation output, not measurement precision.
The allowance excludes the LSE crystal's own frequency tolerance.

Both runs complete and return to idle with the timer stopped and outputs
inactive. The [UART pulse suite](captures/pulse-suite.uart.log) also passes
cases 1–7 with this image, including autonomous underrun stopping. These UART
results check firmware behavior; full independent physical pulse counts are
provided by the instrument captures described above. The clock comparison
does not itself exercise DMA pulse generation.

The results support a main-clock offset relative to the RTC crystal, independent
of the external instruments' timebases or acquisition software. They do not
identify ST-Link's selected MCO source or establish absolute oscillator
calibration. ST-Link's source setting has not been read or changed.

Raw output: [run 1](captures/rtc-reference-1.uart.log),
[run 2](captures/rtc-reference-2.uart.log).
[Clock results](clock-reference-results.json) include the firmware identity,
configuration, per-window values and UART hashes. See the
[reproduction procedure](../../stepper-hardware-test.md#independent-clock-check-c).

## Figures and source measurements

The [figure PDF](step-generator-measurements.pdf) contains the plots for the
current firmware. Individual PNGs:

1. [Pulse counts and final states](01-counts-and-states.png)
2. [Independent rates and completion](02-independent-rates.png)
3. [Maximum-rate waveforms and high widths](03-maximum-rate.png)
4. [Frequency offset and variation](04-clock-offset.png)
5. [Changing-period profile](05-profile.png)
6. [Delayed refill interrupt](06-delayed-interrupt.png)
7. [Autonomous underrun stop](07-underrun-stop.png)
8. [Real counter rollover](08-counter-wrap.png)
9. [Simultaneous Hantek M1/M2 waveforms and frequency](09-oscilloscope-waveforms.png)
10. [Independent instrument frequency measurements](10-independent-timebases.png)
11. [TIM2 against the independent RTC crystal](11-rtc-reference.png)

[measurements.csv](measurements.csv) contains per-axis statistics.
[results.json](results.json) retains measurements, UART results and diagnostic
failures. [provenance.json](provenance.json) identifies sample rates, channel
assignments and the firmware hash. The [captures directory](captures/) holds
the native `.sr` sessions, UART logs, analysis metadata and compressed sigrok
logs. Open the sessions with PulseView or process them with sigrok-cli.

[scope-measurements.csv](scope-measurements.csv) and
[scope-results.json](scope-results.json) contain the Hantek measurements,
analysis window, threshold checks, UART results and capture hashes.

[edges.npz](edges.npz) stores every captured rising and falling edge as an
absolute integer sample index, with keys such as `case-3_D0_rise`. Divide by
the corresponding case's sample rate to obtain seconds. No synthetic or
interpolated edges are used.

To regenerate the figures, using Python with NumPy and Matplotlib:

```sh
python3 docs/measurements/2026-10-03-step-generator/export_results.py
```

The exporter reads the native captures, or uses `edges.npz` and `results.json`
for the logic figures if the capture directory is unavailable. The scope
figures require their native analog captures. For logic acquisition, arm sigrok before
sending one test command over UART; the firmware supplies a two-second delay:

```sh
# Cases 1–7: four seconds at 24 MS/s.
sigrok-cli -d fx2lafw -c samplerate=24MHz --samples 96000000 -o case-N.sr

# Case 8: 440 seconds at 4 MS/s, retaining about 20 samples per high phase.
sigrok-cli -d fx2lafw -c samplerate=4MHz --samples 1760000000 \
  -O binary -o case-8.bin

# Convert the completed raw capture into a sigrok session.
sigrok-cli -I binary:numchannels=8:samplerate=4000000 -i case-8.bin -o case-8.sr
```

Use direct binary output for the long acquisition to avoid repeatedly
updating a growing session archive while sampling. The raw file uses one byte
per sample (D0 in bit 0 through D7 in bit 7) and occupies 1.76 GB. Session
packaging is lossless and must retain every byte and the 4 MHz sample rate.
The stored case-8 session is packed from the raw stream; its metadata records
the raw-byte SHA-256 so the conversion can be checked independently.

For the scope, start case 3 over UART, then acquire during its one-second
pulse train following the two-second preparation delay. The capture command
for 16 MS/s is:

```sh
sigrok-cli -d hantek-6xxx -C CH1,CH2 -c samplerate=16MHz \
  -c channel_group=CH1:vdiv=100mV -c channel_group=CH2:vdiv=1V \
  --samples 1048576 -o hantek-16mhz.sr
```

Use `samplerate=8MHz` for the other scope configuration. The scope driver does
not provide a hardware edge trigger here; acquisition is timed from the UART
command, and the retained captures are interior segments. The firmware remains
idle with drivers disabled following each completed test.

## Scope

These are unloaded Nucleo STEP measurements. They establish the recorded
pulse counts and stopping behavior under the exercised conditions. The
25 scheduler/register-model tests also pass, including sanitizer builds. The
clock diagnostic adds six host checks for rate calculation, uncertainty,
counter/minute rollover and invalid sampling; all 31 host checks pass.

The measurements do not establish transistor/driver propagation, calibrated
voltage or signal-integrity limits, actual motor motion, encoder feedback,
homing, or operation under concurrent Ethernet/storage stress. Clock accuracy
remains an open item.
