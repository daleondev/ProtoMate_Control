# STEP generator measurements

The current `step-test-stm32` firmware produces the requested finite pulse
counts on all three outputs. Independently captured counts also match the
firmware's counts for software abort and autonomous underrun stop.
**Absolute timing accuracy remains unresolved:** a requested 100.000 kHz
measures **100.643 kHz**, approximately **0.643% fast relative to the logic
analyzer**. These measurements do not establish overall timing conformance.

Measurements: 2026-10-03. The test firmware remains programmed on the Nucleo;
its shared driver enable is inactive and it waits for a serial command.

## Setup and firmware

| Item | Configuration |
| --- | --- |
| Controller | NUCLEO-H753ZI, production TIM2/DMA STEP generator |
| Test image | `step-test-stm32`, optimized build with debugger symbols; OpenOCD programming verified |
| Instrument | AZDelivery 8-channel logic analyzer, `fx2lafw`, `sigrok-cli 0.7.2` |
| Cases 1–7 | 24 MS/s, 41.667 ns sample spacing, 96,000,000 samples / 4 s per capture |
| Case 8 | 4 MS/s, 250 ns sample spacing, 1,760,000,000 samples / 440 s; resolves each nominal 5 µs high phase |
| Serial | ST-Link virtual COM port, 115200 baud, 8N1 |
| Driver controls | Firmware holds PE15 / EN_N high and DIR outputs low |
| Timer | Nominal 240 MHz input / 24 = 10 MHz counter, `ARR = 0xFFFFFFFE` |

Programmed `Application.bin` SHA-256:

```text
b57c9aa969940a73cbe1300cfb11463e8fcdd6f228e4b7e2c7dc35bd99ce2c6f
```

The [programming log](firmware-programming.log) records successful flash verification.

The [bench source](../../../platform/hal/tests/hardware_step_test.cpp) and
[manual procedure](../../stepper-hardware-test.md) define the test cases.

| Analyzer input | Signal | Nucleo contact |
| --- | --- | --- |
| D0 | M1 STEP, PA0 | CN10 pin 29 |
| D1 | M2 STEP, PB10 | CN10 pin 32 |
| D2 | M3 STEP, PB11 | CN10 pin 34 |
| Ground | GND | CN10 pin 22 |

Case 1's distinct 1/2/3 counts confirm the STEP channel mapping. D3 is high
throughout the captures, but its physical connection to the optional EN_N
probe is not independently established. Driver-control state is checked by
the firmware. No analog voltage measurement is included.

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

The firmware assumes an 8 MHz HSE bypass clock from ST-Link. ST documents
an HSI/2 8 MHz STLINK-V3 MCO option and crystal-derived MCO alternatives.
This makes the clock source a plausible contributor to the observed error;
its selection and actual frequency were not measured independently. Both the
MCU clock and the analyzer timebase remain uncalibrated. See
[ST's MCO clock-source guidance](https://community.st.com/stm32-mcus-60/how-to-use-stlink-v3-mco-output-on-nucleo-boards-as-a-precise-clock-source-for-stm32-140173).

The outstanding timing work is to verify the clock against a suitable
reference, establish the required accuracy, and coordinate any clock-source
selection with the CubeMX HSE/PLL configuration before remeasurement.

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

[measurements.csv](measurements.csv) contains per-axis statistics.
[results.json](results.json) retains measurements, UART results and diagnostic
failures. [provenance.json](provenance.json) identifies sample rates, channel
assignments and the firmware hash. The [captures directory](captures/) holds
the native `.sr` sessions, UART logs, analysis metadata and compressed sigrok
logs. Open the sessions with PulseView or process them with sigrok-cli.

[edges.npz](edges.npz) stores every captured rising and falling edge as an
absolute integer sample index, with keys such as `case-3_D0_rise`. Divide by
the corresponding case's sample rate to obtain seconds. No synthetic or
interpolated edges are used.

To regenerate the figures, using Python with NumPy and Matplotlib:

```sh
python3 docs/measurements/2026-10-03-step-generator/export_results.py
```

The exporter reads the native captures, or uses `edges.npz` and `results.json`
if the capture directory is unavailable. For acquisition, arm sigrok before
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

## Scope

These are unloaded Nucleo STEP measurements. They establish the recorded
pulse counts and stopping behavior under the exercised conditions. The
25 scheduler/register-model tests also pass, including sanitizer builds.

The measurements do not cover transistor/driver propagation, analog signal
integrity, actual motor motion, encoder feedback, homing, or operation under
concurrent Ethernet/storage stress. Clock accuracy remains an open item.
