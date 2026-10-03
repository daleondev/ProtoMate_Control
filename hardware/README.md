# ProtoMate hardware

Open **[ProtoMate.kicad_pro](ProtoMate.kicad_pro)** in KiCad 10. This is the single
hardware project: the complete system schematic and the linked perfboard layout
share the same project name and component identities.

```text
hardware/
├── ProtoMate.kicad_pro       Project settings and electrical checks
├── ProtoMate.kicad_sch       Root sheet: controller, power, sheet navigation
├── ProtoMate.kicad_pcb       100 × 160 mm perfboard placement and hand wiring
├── sheets/                  Motor interfaces, encoder, storage, cable headers
├── libraries/               Project-local symbols and perfboard footprints
├── sym-lib-table            Relative symbol-library registration
├── fp-lib-table             Relative footprint-library registration
├── assembly/                Build instructions, parts, hole/wire/harness tables
├── exports/                 Schematic/assembly PDFs and verification reports
└── tools/                   Repeatable checks and export command
```

## Schematic and layout relationship

The root schematic links five child sheets: M1/DM542T, M2/M3/TMC2209,
M1 encoder, storage, and perfboard cable headers. Global net names connect
these sheets electrically. The connector sheet explicitly defines J101–J107;
its pin numbers match the layout and [harness table](assembly/Harness.csv).

All **40 perfboard footprints** have assigned library footprints and native
links to their schematic symbols. These comprise Q1–Q6, R1–R12, R20–R28,
C3–C6, U2/U3 and J101–J107. R10–R12 remain on the layout and are marked DNP.

The Nucleo, motor drivers, motors, buck converter, storage modules and their
external components are marked **Exclude from board** in the schematic.
They remain part of the system wiring and system parts list, but KiCad does
not add them to the perfboard. In particular, 24 V and motor currents stay
outside the perfboard.

The `.kicad_pcb` is a map for a purchased individual-pad board: F.Cu represents
top jumpers, B.Cu represents solder-side wires, and vias represent wire passages.
Use the [assembly instructions](assembly/README.md) to build it.

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
harness destinations. It updates:

- [Wiring.pdf](exports/Wiring.pdf): complete six-sheet system schematic.
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
