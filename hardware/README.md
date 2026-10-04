# ProtoMate hardware

Open **[ProtoMate.kicad_pro](ProtoMate.kicad_pro)** in KiCad 10. This is the single
hardware project: the complete system schematic and the linked perfboard layout
share the same project name and component identities.

```text
hardware/
├── ProtoMate.kicad_pro       Project settings and electrical checks
├── ProtoMate.kicad_sch       Root sheet: controller, power, sheet navigation
├── ProtoMate.kicad_pcb       39 × 48 hole perfboard placement and hand wiring
├── sheets/                  Motor interfaces, encoder, storage, headers, switches
├── libraries/               Project-local symbols, footprints and buck 3D model
├── sym-lib-table            Relative symbol-library registration
├── fp-lib-table             Relative footprint-library registration
├── assembly/                Build instructions, parts, hole/wire/harness tables
├── exports/                 Schematic/assembly PDFs and verification reports
└── tools/                   Repeatable checks and export command
```

## Schematic and layout relationship

The root schematic links six child sheets: M1/DM542T, M2/M3/TMC2209,
M1 encoder, storage, perfboard cable headers, and reference limit switches.
Global net names connect these sheets electrically. The connector sheet defines
J101–J107 and the reference-switch sheet defines J108–J111; their pin numbers
match the layout and [harness table](assembly/Harness.csv).

All **44 perfboard footprints** have assigned library footprints and native
links to their schematic symbols. These comprise Q1–Q6, R1–R12, R20–R28,
C3–C6, U2/U3 and J101–J111. R10–R12 remain on the layout and are marked DNP.

The **buck U1 and Adafruit A3/A4 modules mount directly at J103/J105/J106**.
These footprints represent their soldered mounting pins and full module
outlines. The module symbols remain excluded from separate PCB placement:
the DC jack and JP1 power/motor terminals are on the modules, not additional
perfboard holes. The system parts table identifies their physical mounting
through these headers. J103 has two output contacts and two isolated VIN
support islands; its no-connect flags mean **no perfboard wire**, not no voltage.
The Nucleo, DM542T, motors, switches and storage modules remain external.
Motor power and winding cables connect directly to the drivers; their currents
do not use the perfboard wiring. The buck's 63 × 27 mm body follows the supplied
STEP model; its nominal mounting grid is 5.08 mm within each pair and 50.8 mm
between pairs. Its barrel jack receives 24 V at the left edge.

The `.kicad_pcb` is a map for a purchased individual-pad board: F.Cu represents
top jumpers, B.Cu represents solder-side wires, and vias represent wire passages.
It uses one continuous **39-column × 48-row grid**, A1–AV39, at 2.54 mm pitch.
The outer hole centres span **96.52 × 119.38 mm**; the nominal outline is
99.06 × 121.92 mm with half-pitch margins. Both TMC modules sit along the top
edge, the buck at the lower left, and the reference headers at the lower right.
Wiring uses 96 underside-only connections and 25 short crossovers through
dedicated free holes. All wire ends are soldered underneath. J101's pin 1 is
at I9, with its signal column facing the driver modules.
Use the [assembly instructions](assembly/README.md) to build it.

The STEP engine uses one 32-bit TIM2 with DMA: M1 = PA0/CN10.29,
M2 = PB10/CN10.32, M3 = PB11/CN10.34 (channels 1/3/4, all AF1).
DIR uses PE12/CN10.26, PE13/CN10.10, PE14/CN10.8; the shared active-low
enable is PE15/CN10.30. These signals use J101 pins 1/3/5/7/9/11/13.
The harness table, Nucleo symbol and layout notes specify the cable destinations.
Keep PA0's SB75 ON and the user
button on PC13. TIM5 is the runtime clock; TIM7 provides an internal finite-move
completion check and uses no connector pin. PE9/PE11 (TIM1) and PC6/PC7
(TIM8) are reserved for possible additional encoders, with no cables fitted.

Set the onboard **STLINK-V3 MCO to HSE/5 (5 MHz)**. CubeMX uses that external
clock in bypass mode for a 480 MHz CPU, 240 MHz timer kernels and 10 MHz STEP
counter. This persistent ST-Link setting is configured separately from CubeMX;
follow the [clock setup procedure](../docs/stepper-hardware-test.md#clock-source-configuration)
when preparing a board.

The three reference-switch cables connect to J109 (M1), J110 (M2), and J111
(M3), each with pin 1 = REF and pin 2 = GND. J108 returns the signals to
PE7/CN10.20, PE8/CN10.18 and PE10/CN10.24 on odd pins 1/3/5; even pins
2/4/6 connect to Nucleo GND/CN10.22. This is passive wiring using the
firmware's internal 3.3 V pull-ups. SW1–SW3 are shown released with COM–NC
closed: low when released, high when pressed or unplugged. Their schematic
contact names are functional identities, not an asserted Creality connector
pin order; verify the normally closed pair by continuity before making cables.

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
python3 hardware/tools/refresh.py
```

Requires KiCad 10 (`kicad-cli` and its `pcbnew` Python module), Python 3 and
Poppler's `pdfunite`. The command checks ERC, DRC **with schematic parity**,
symbol links, pad nets, the hole grid, wire endpoints/connectivity, module body
clearance, mounting contacts and harness destinations.
All 26 motor, encoder, reference and storage signal contacts
are checked against CubeMX, along with reference pull-ups/edges, encoder index
polarity and the disabled startup level. It updates:

- [Wiring.pdf](exports/Wiring.pdf): complete seven-sheet system schematic.
- [Assembly.pdf](exports/Assembly.pdf): placement, top wires, mirrored underside
  and combined overview, at actual size.
- [Holes.csv](assembly/Holes.csv) and [System_Parts.csv](assembly/System_Parts.csv):
  generated pin locations and system components, including board/external scope.
- [ERC.rpt](exports/ERC.rpt), [DRC.rpt](exports/DRC.rpt) and
  [Verification.txt](exports/Verification.txt): checks of the saved native files.

Use `python3 hardware/tools/refresh.py --check-only` to refresh just the checks.
The tool reads the native design; it never regenerates or overwrites its
schematic or layout. Wire lengths and side choices in the assembly schedule
remain manually reviewed when routing changes. The checks validate a wiring
model, not solder joints or measured encoder-interface timing.

Manufacturing silkscreen and missing-courtyard checks are waived for this
hand-wiring map. Electrical clearance, unconnected pads, schematic parity and
local footprint-library consistency are checked. No fabrication outputs are
generated.
