# ProtoMate stepper perfboard

Open **[ProtoMate.kicad_pro](ProtoMate.kicad_pro)** in KiCad 10. The complete
system schematic and linked perfboard layout share the same project name and
component identities.

The controller is the **Arduino GIGA R1 WiFi / STM32H747XIH6**. Its M4
owns motor generation, feedback, reference switches and driver diagnostics.
The physical perfboard uses four cable harnesses to the GIGA; it is a separate
board, not the display shield. Controller destinations use the Arduino's
printed **D/A labels**, checked against the
[official pinout](https://docs.arduino.cc/resources/pinouts/ABX00063-full-pinout.pdf)
and the repository's current CubeMX configuration.

Power the GIGA independently through USB-C. Its **3V3 output feeds J101.8**;
ground is common with the perfboard. **Do not connect the buck's 5 V output to
GIGA 5V, VIN or USB.** The buck powers the DM542T signal interface and the
encoder's 5 V circuitry; 24 V supplies the three motor drivers.

```text
hardware/perfboard/
├── ProtoMate.kicad_pro       Project settings and electrical checks
├── ProtoMate.kicad_sch       Root sheet: controller, power, sheet navigation
├── ProtoMate.kicad_pcb       47 × 65 hole perfboard placement and hand wiring
├── sheets/                  Motor interfaces, encoder, headers, switches
├── libraries/               Project-local symbols, footprints and buck 3D model
├── sym-lib-table            Relative symbol-library registration
├── fp-lib-table             Relative footprint-library registration
├── assembly/                Build instructions, parts, hole/wire/harness tables
├── exports/                 Schematic/assembly PDFs and verification reports
└── tools/                   Repeatable checks and export command
```

## Schematic and layout relationship

The root schematic links five child sheets: M1/DM542T, M2/M3/TMC2209,
M1 encoder, perfboard cable headers, and reference limit switches.
Global net names connect these sheets electrically. The connector sheet defines
J101–J107 and J113, and the reference-switch sheet defines J108–J111.
J104 is the ten-way DM542T screw terminal for control, ALM and 24 V power.
The M1 sheet defines pull-up R30; J101 carries ALM to the GIGA. All connector pin numbers match the layout and
[harness table](assembly/Harness.csv).

All **49 perfboard footprints** have assigned library footprints and native
links to their schematic symbols. These comprise Q1–Q6, R1–R12, R20–R30,
C1–C6, U2/U3, J101–J111 and J113. R10–R12 remain on the layout and are marked DNP.

The **buck U1 and Adafruit A3/A4 modules mount directly at J103/J105/J106**.
These footprints represent their soldered mounting pins and full module
outlines. The module symbols remain excluded from separate PCB placement:
the barrel jack and four-way motor terminals remain on their modules.
J103 has four electrical contacts: 5 V output, output ground, VIN ground and
24 V input. J105/J106 each have ten JP4 control pins plus two JP1 power pins.
J104.9/10 provide the DM542T power connection and the two power-distribution
star joints. Dedicated underside supply/return branches feed each TMC module
through its local C1/C2 bulk capacitor connections. Motor winding cables
connect directly to the drivers. The GIGA, DM542T, motors and switches remain external. The buck's 63 × 27 mm body follows the supplied
STEP model; its nominal mounting grid is 5.08 mm within each pair and 50.8 mm
between pairs. Its barrel jack receives 24 V at the left edge.

J113 uses eight contacts for six signals and two shared ground returns to the GIGA: TX (PD5),
RX (PD6), M2 DIAG (PC3), M3 DIAG (PC0), M2 INDEX (PB8) and M3 INDEX (PB9). R29 is the shared UART's 1 kΩ TX
series resistor. J105/J106 pin 9 is UART, pin 7 is DIAG and pin 8 is INDEX.
Address straps are M2 MS1/MS2 = GND/GND and M3 = 3.3 V/GND. Both SPRD jumpers
stay open. Firmware configures current and 16 microsteps before enable; the
potentiometers do not set current in UART mode. See the
[UART wiring instructions](assembly/README.md#directly-mounted-tmc2209-modules).

The `.kicad_pcb` is a map for a purchased individual-pad board: F.Cu represents
top jumpers, B.Cu represents solder-side wires, and vias represent wire passages.
Use the **65 × 47-hole board**, oriented as **47 columns × 65 rows**, A1–BM47,
at 2.54 mm pitch. The outer hole centres span **116.84 × 162.56 mm**; the
nominal outline is 119.38 × 165.10 mm with half-pitch margins. J104 and both TMC modules
sit along the top edge with their cable entries facing outward. J107 sits below
J104, keeping both motor-1 cables together. J101, J113, J102 and J108 form a
left-edge GIGA connector bank with 9.1 mm gaps between bodies and pin 1 at
the bottom of every header. The encoder receiver and level converters occupy
the centre-left; the DM542T transistor stages occupy the right interior.
The buck sits at the bottom left, with the three switch cable headers at the
bottom right. Q1–Q3 have 30.48 mm between stages; Q4–Q6 have
15.24 mm between stages. Decoupling capacitors stay close to their ICs.
The barrel jack faces the left edge.
The wire schedule specifies underside routes and insulated top crossovers
through dedicated free holes. All wire ends are soldered underneath. J101's pin 1 is
at Z4; odd pins run up column 4 and even pins up column 3. The 11 designated
power connections all stay underneath and use 0.5 mm² insulated copper wire;
wire drawing width does not specify conductor size.
Use the [assembly instructions](assembly/README.md) to build it.

The STEP engine uses one 32-bit TIM2 with DMA: channels 1/3/4, all AF1,
with a 10 MHz timeline. M4's runtime uses TIM5, TIM6 samples the encoder and
TIM7 services STEP completion. The GIGA's own 16 MHz crystal provides the clock;
there is no external ST-Link clock setting or board solder-bridge change.

| Signal | Perfboard contact | GIGA label | MCU pin / function |
| --- | --- | --- | --- |
| M1 STEP | J101.1 | A7 | PA0 / TIM2_CH1 |
| M1 DIR | J101.3 | D4 | PJ8 |
| M1 ALM | J101.4 | A3 | PB1 / EXTI1 |
| M2 STEP | J101.5 | D3 | PA2 / TIM2_CH3 |
| M2 DIR | J101.7 | D6 | PD13 |
| M3 STEP | J101.9 | D2 | PA3 / TIM2_CH4 |
| M3 DIR | J101.11 | D16 | PH13 |
| Shared enable | J101.12 | D15 | PC7; HIGH = disabled |
| Logic supply | J101.8 | 3V3 | GIGA output |
| M1 encoder A | J102.1 | D7 | PB4 / TIM3_CH1 |
| M1 encoder B | J102.3 | D5 | PA7 / TIM3_CH2 |
| M1 encoder Z | J102.5 | D53 | PG7 / EXTI7 |
| M1 reference | J108.1 | D49 | PE4 / EXTI4 |
| M2 reference | J108.2 | D51 | PE5 / EXTI5 |
| M3 reference | J108.3 | D47 | PB2 / EXTI2 |
| TMC UART TX | J113.1 | D18 | PD5 / USART2_TX |
| TMC UART RX | J113.3 | D19 | PD6 / USART2_RX |
| M2 INDEX | J113.4 | D8 | PB8 / EXTI8 |
| M2 DIAG | J113.5 | A4 | PC3 / EXTI3 |
| M3 INDEX | J113.6 | D9 | PB9 / EXTI9 |
| M3 DIAG | J113.7 | A6 | PC0 / EXTI0 |

All nine controller ground returns join the GIGA's common GND. See
[Harness.csv](assembly/Harness.csv) for each return and the complete driver,
module and switch wiring. The GIGA's QSPI flash is onboard and the filesystem
uses internal flash; no external QSPI or SD wiring is part of this project.

R7's 470 Ω pull-up holds shared enable HIGH during reset. M2/M3 STEP have
10 kΩ pull-downs R8/R9; M1 STEP has a path to GND through R1 and R2. The
firmware keeps STEP LOW and enable HIGH until motion is explicitly enabled.
These physical bias paths depend on the perfboard and its GIGA logic supply
being connected.

The reference switches connect to J109/J110/J111 (M1/M2/M3), pin 1 = REF and
pin 2 = GND. COM–NC is closed when released: LOW is healthy/released; HIGH
means pressed or an open cable. The GIGA supplies internal 3.3 V pull-ups
and uses both-edge EXTI. The schematic names functional switch contacts;
check the NC pair by continuity rather than relying on connector colours.

DM542T ALM+/ALM− connect to J104.7/8. R30 pulls ALM+ to 3.3 V through 4.7 kΩ;
J101.4 returns it to A3/PB1. The normally conducting alarm is LOW when healthy
and HIGH on fault/open cable. See the
[external connection instructions](assembly/README.md#external-connections).

## Controller harness

Use four short harnesses between the GIGA and perfboard: J101 (12 contacts),
J102 (6), J108 (4), and J113 (8). Each signal appears exactly once; 3.3 V
enters only at J101.8. J101 also carries the M1 alarm signal.

The nine ground conductors have deliberate roles: three STEP returns at J101,
three encoder returns at J102, one shared switch return at J108, and two shared
UART/diagnostic returns at J113. Twist each STEP and encoder signal with its
assigned ground. Bundle the slower signals with their shared returns; keep these
short local harnesses separate from motor and 24 V cables. The ground conductors
may join at a ground bus beside the GIGA; they do not need separate GIGA GND
pins. Use a short, solid connection from that bus to GIGA GND.

Module ground contacts and address straps remain connected as required by the
modules. Motor supply current uses the dedicated power branches. Ground links
in the perfboard wire schedule are required even where cable returns are shared.

## Editing workflow

1. Open the project above, then its schematic and PCB editors. Edit electrical
   connections and component assignments in the schematic.
2. Save the schematic and use **Tools → Update PCB from Schematic (F8)**.
   Keep UUID-based association; leave **Re-link footprints to schematic symbols
   based on their reference designators** unchecked. Review the proposed changes.
3. Apply the update, then place new parts and adjust the hand-wire routes in the
   PCB editor. Use the 2.54 mm hole grid for leads and wire passages. F8 transfers
   connectivity and component changes; it does not design the new wiring routes.
4. Maintain [Wires.csv](assembly/Wires.csv) after routing changes and
   [Harness.csv](assembly/Harness.csv) after cable/connector changes. Review
   mounting details in [Parts.csv](assembly/Parts.csv). Run the refresh command
   below before using or sharing the assembly documents.

Project-local footprints preserve the actual grid spacing and Diotec E/B/C
pad identities. They resolve through `${KIPRJMOD}`, so moving or cloning the
repository does not require personal library paths. The X/Y axial resistor
patterns describe the two existing orientations of the assembly drawing.

There is no separate netlist-import step or second electrical design to maintain.
KiCad's schematic-to-board check verifies the association, footprint identity,
value, and pad connectivity in the saved project.

## Checks and exports

From the repository root:

```sh
python3 hardware/perfboard/tools/refresh.py
```

Requires KiCad 10 (`kicad-cli` and its `pcbnew` Python module), Python 3 and
Poppler's `pdfunite`. The command checks ERC, DRC **with schematic parity**,
symbol links, pad nets, the hole grid, wire endpoints/connectivity, module body
clearance, mounting contacts, dedicated power branches and harness destinations.
All 20 motor, encoder, reference and driver signal contacts are checked against
the GIGA schematic and **external/CubeMX/CubeMX.ioc by default**: MCU pin,
peripheral, M4 ownership, firmware label, input pulls/edges, UART settings and
disabled startup level. The checks also reject buck 5 V or external-storage
connections on the controller. Use `--ioc /path/to/GIGA.ioc` to select another
matching GIGA configuration. The command updates:

- [Wiring.pdf](exports/Wiring.pdf): complete six-sheet system schematic.
- [Assembly.pdf](exports/Assembly.pdf): placement, top wires, mirrored underside
  and combined overview, at actual size.
- [Holes.csv](assembly/Holes.csv) and [System_Parts.csv](assembly/System_Parts.csv):
  generated pin locations and system components, including board/external scope.
- [ERC.rpt](exports/ERC.rpt), [DRC.rpt](exports/DRC.rpt) and
  [Verification.txt](exports/Verification.txt): checks of the saved native files.

Use `python3 hardware/perfboard/tools/refresh.py --check-only` to refresh just the checks.
The tool reads the native design; it never regenerates or overwrites its
schematic or layout. Wire lengths and side choices in the assembly schedule
remain manually reviewed when routing changes. The checks validate a wiring
model, not solder joints or measured encoder-interface timing.

Manufacturing silkscreen and missing-courtyard checks are waived for this
hand-wiring map. Electrical clearance, unconnected pads, schematic parity and
local footprint-library consistency are checked. No fabrication outputs are
generated.
