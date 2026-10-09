# LAN9255 SPI feasibility test

`ethercat-test-stm32` runs on the NUCLEO-H753ZI and tests direct SPI access to
the LAN9255's LAN9253 EtherCAT controller. The SAM program flash stays erased.
This stage covers STM32 loopback, volatile ESC boot configuration, identification,
and repeated read reliability.
SOES, EtherCAT state transitions, EEPROM programming and process RAM tests are
later stages.

Only the Nucleo, EVB, their USB power connections and common ground are needed.
No motors, encoders, reference switches, SD card or external flash are required.
The image skips storage initialization, motor timers/UART and Ethernet startup.
The shared motor enable stays inactive (PE15 high). SPI5 is initialized when
the test creates the bus; no SPI transfers start until a console command.

## Build and flash

```sh
cmake --preset ethercat-test-stm32
cmake --build --preset ethercat-test-stm32
openocd -f interface/stlink.cfg -f target/stm32h7x_dual_bank.cfg \
  -c "gdb_port disabled; tcl_port disabled; telnet_port disabled" \
  -c "program build/ethercat-test-stm32/Application.elf verify reset exit"
```

Flash through the Nucleo's ST-Link, not the EVB's Atmel-ICE. The existing
ST-Link crystal-derived **5 MHz MCO** setting and target clock configuration
are retained. Open the ST-Link virtual COM port at **115200 baud, 8N1, no flow
control**. Commands accept either CR or LF. The banner reports `937500 Hz` and
`READY`; type `help` if a terminal connected after startup missed the banner.

## First: Nucleo loopback

Leave all EVB SPI wires disconnected. Connect one temporary jumper:

**CN9 pin 28 (PF9 / MOSI) ↔ CN9 pin 24 (PF8 / MISO).**

Run `loopback`. Expect `PASS` after **288 exchanges**, covering fixed and
deterministic random data, 1–257-byte transfers and unaligned buffer offsets.
Chip select remains high throughout loopback. Failure does not latch the test;
fix the jumper and retry. Remove this jumper before connecting the ESC.

## Then: EVB connection and straps

Power down before wiring or changing straps. Keep the SAM erased, disconnect
Atmel-ICE from the EVB, and set **J17 EE_EMUL[2:0] = 000** for ordinary SPI.
There are four jumpers because EMUL0 is duplicated; put **both EMUL0 jumpers**
at zero. The four zero-position pairs are **1–2, 4–5, 7–8, and 10–11**.
Power-cycle the EVB after a strap change. The `100` setting selects a different
register map and is not the mode used by this test.

`000` also enables **EEPROM emulation**: it selects the initial SPI interface,
but the host must supply the ESC boot configuration. With the SAM erased,
`BYTE_TEST` responds while `HW_CFG.READY` remains zero until the STM32 runs
`boot`. The physical EEPROM is not used in this mode.

Use short 3.3 V logic connections, ideally about 10 cm. Power each board from
its own USB connection and connect their grounds; do not join their supply pins.
J3 takes a 1×8, 2.54 mm header. Numbers below are **physical connector pin
numbers**, not Arduino D-number labels.

| Signal | Nucleo | EVB | Optional analyzer |
|---|---|---|---|
| SCK | PF7, **CN9.26** | **J3.1** | D0 |
| CS active low | PF6, **CN10.11** | **J3.3** | D1 |
| MOSI | PF9, **CN9.28** | **J3.4** (SIO0) | D2 |
| MISO | PF8, **CN9.24** | **J3.5** (SIO1) | D3 |
| GND | **CN9.23** | **J3.2** (or J3.8) | GND |

J3.6 and J3.7 are unused. IRQ, SYNC0 and reset are not needed or driven by
this test. Both boards must be powered while exchanging data. For analyzer
verification, select **24 MHz**, SPI mode **0**, **8 bits**, **MSB first**,
and active-low chip select. This analyzer mapping replaces the earlier
erased-SAM startup measurement mapping.

## Commands and acceptance

| Command | Behavior |
|---|---|
| `help` | Wiring reminders and command list. |
| `status` | Busy/idle, completed count, SPI clock and last successful register values. No SPI traffic. |
| `loopback` | Nucleo-only MOSI-to-MISO comparison; CS stays high. |
| `probe` | Poll for up to 2 seconds; require BYTE_TEST, READY, then the correct chip ID. |
| `boot` | Serve the 16-byte emulated boot configuration, then require READY and the chip ID; bounded to 1 second. Already-ready devices are only identified. |
| `stress` | 100,000 consecutive checked register sets; no retries during the run. |
| `stress N` | Select 1–1,000,000 checked sets. |
| `stop` | Cancel the current operation after its bounded transfer and release CS. |

After each EVB power-up, run **`boot`, `probe`, then `stress`**. Identification must report:

- `BYTE_TEST = 0x87654321` at **0x0064**.
- `HW_CFG.READY = 1` (bit 27) at **0x0074**.
- `ID_REV[31:16] = 0x9253` at **0x0050**; the low 16 bits are the silicon revision
  (the connected EVB reports `0x92530000`).

`boot` serves the two 8-byte configuration requests through the indirect ESC
CSR interface. It uses the following test-only configuration, including CRC-8:

```text
80 00 00 00 00 00 00 00 00 00 00 00 00 00 E9 00
```

This selects ordinary SPI (`0x80`) and leaves GPIOs as inputs, device emulation
disabled and SYNC outputs unconfigured. It is **not a complete SII description**
and does not bring up EtherCAT networking. Configuration is volatile: SAM flash
and the physical EEPROM are untouched. Non-emulation mode, EEPROM writes and
requests outside this 16-byte header are rejected. Resetting the Nucleo alone
does not erase configuration already loaded into the separately powered ESC.

Each stress set checks all three registers in that order, including a stable
ID/revision. The default therefore checks **300,000 individual register reads**.
Any transfer error, wrong byte-test value, missing READY or changed identity
fails the run immediately; `status` gives the completed count. A completed
run prints `PASS` and returns to `READY; CS high`. Repeat `boot`, `probe` and `stress`
after EVB power cycles to verify independent startup. Do not switch off the EVB
during a success run; testing interrupted service is a separate recovery test.

For an analyzer check, use `probe` or `stress 10`. A BYTE_TEST read sends:

```text
MOSI: 03 00 64 00 00 00 FF
MISO: xx xx xx 21 43 65 87
```

There are 56 clocks under one CS assertion. Address bytes are MSB first, data
bytes LSB first, and the last transmitted `FF` terminates the READ prefetch.
The default READ command requires no dummy byte. SPI uses polling transfers
with finite timeouts, not DMA; ordinary stack buffers work with D-cache enabled.
CS is released on success, cancellation, timeout and exception. A failed HAL
transfer resets the SPI peripheral so the next command can recover.

If BYTE_TEST is wrong, check common ground, MOSI/MISO, the removed loopback
jumper and J17 first. `0x00000000` or `0xFFFFFFFF` alone is not identification.
If BYTE_TEST is correct but READY is zero, SPI is responding and ESC startup
is incomplete; with J17=`000`, run `boot`. The test reports this separately
from a transport failure. `probe` and `stress` only read system registers and
never read ID_REV until BYTE_TEST and READY pass. Only `boot` writes the CSR
command/data registers to service pending emulated configuration requests.

## Implementation and checks

The board creates an exclusive `ISpi` transport and a separate GPIO chip
select. `hal::board::createEthercatSpi()` selects `hal::spi::create({ .peripheral = hal::spi::Peripheral::Spi5 })`;
the concrete `hal::Spi` declaration lives in each platform's `Spi.hpp`,
with construction and peripheral selection in the driver factory.
The SPI driver has no EtherCAT protocol or device configuration.
`hal::device::Lan9253` owns the read framing and readiness/identity
checks. One bench worker owns all transactions; the console can report status
and request cancellation while it runs.

Linux HAL tests cover byte order, the final `FF`, chip-select release on errors
and exceptions, alignment/range rejection, readiness ordering, wrong identity,
retry and the shared identification timeout. Additional tests cover CSR framing,
alignment and busy timeouts, the two boot requests, CRC rejection, idempotence,
and rejection of physical EEPROM mode, write commands and out-of-range requests.
The STM32 build validates the
generated SPI5 assignment and the existing clock/motor configuration.

### Hardware verification (2026-10-09)

With the SAM flash erased, J17=`000` and SPI5 at 937,500 Hz:

- `boot` loads the volatile configuration and identification reports
  `BYTE_TEST=87654321`, `HW_CFG=0800001B`, `ID_REV=92530000`.
- Two runs of 100,000 checked sets passed, totaling **600,000 system-register
  reads with zero mismatches**, in 20.325 s and 20.337 s respectively.
- The second run followed a user-confirmed EVB power cycle. READY was zero
  before `boot` and set afterwards. Repeating `boot` on an already-ready ESC
  also passed.
- Resetting only the Nucleo preserved ESC readiness. Identification passed,
  a stress run was cancelled cleanly, and another 1,000 sets passed afterwards.

These results establish SPI communication and the minimal emulated boot
sequence. They do not establish process-RAM operation, physical EEPROM boot,
EtherCAT networking or sustained cyclic PDO exchange.

Sources: [EVB guide, §3.1.6 and schematics](EVB-LAN9255-Evaluation-Board-Users-Guide-50003106.pdf),
[LAN9253 datasheet, §§6.4, 9.2.1, 9.2.5.1, 11.15, 12.2, 13.4 and 15.1](https://ww1.microchip.com/downloads/aemDocuments/documents/UNG/ProductDocuments/DataSheets/LAN9253-Data-Sheet-DS00003421.pdf).
