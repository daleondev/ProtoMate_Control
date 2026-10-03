# ProtoMate hardware

Open **[ProtoMate.kicad_pro](ProtoMate.kicad_pro)** in KiCad 10. This is the single
hardware project: the complete system schematic and the linked perfboard layout
share the same project name and component identities.

```text
hardware/
├── ProtoMate.kicad_pro       Project settings and electrical checks
├── ProtoMate.kicad_sch       Root sheet: controller, power, sheet navigation
├── ProtoMate.kicad_pcb       100 × 160 mm perfboard placement and hand wiring
├── sheets/                  Motor interfaces, encoder, storage, headers, switches
├── libraries/               Project-local symbols and perfboard footprints
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

The Nucleo, motor drivers, motors, reference switches, buck converter, storage modules and their
external components are marked **Exclude from board** in the schematic.
They remain part of the system wiring and system parts list, but KiCad does
not add them to the perfboard. In particular, 24 V and motor currents stay
outside the perfboard.

The `.kicad_pcb` is a map for a purchased individual-pad board: F.Cu represents
top jumpers, B.Cu represents solder-side wires, and vias represent wire passages.
Use the [assembly instructions](assembly/README.md) to build it.

The STEP engine uses one 32-bit TIM2 with DMA: M1 = PA0/CN10.29,
M2 = PB10/CN10.32, M3 = PB11/CN10.34 (channels 1/3/4, all AF1).
DIR uses PE12/CN10.26, PE13/CN10.10, PE14/CN10.8; the shared active-low
enable is PE15/CN10.30. These signals retain J101 pins 1/3/5/7/9/11/13
and their existing perfboard nets. The harness table, Nucleo symbol and layout
notes specify the new cable destinations. Keep PA0's SB75 ON and the user
button on PC13. TIM5 is the runtime clock; PE9/PE11 (TIM1) and PC6/PC7
(TIM8) are reserved for possible additional encoders, with no cables fitted.

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

This replaces the former independent `wiring/` and `perfboard/` projects.
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
symbol links, pad nets, the hole grid, wire endpoints/connectivity and external
harness destinations, including the seven motor GPIOs against CubeMX. It updates:

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
generated. Close any old project/editor windows and reopen the single project
at the path above after this migration.
