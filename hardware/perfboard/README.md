# Perfboard assembly

Open [ProtoMate_Perfboard.kicad_pro](ProtoMate_Perfboard.kicad_pro) in KiCad 10,
then open the PCB editor. This file is a **placement and hand-wiring map for a
purchased 100 × 160 mm individual-pad perfboard**. The tracks represent wires;
there are no Gerber files or custom-board fabrication instructions.

[Assembly.pdf](Assembly.pdf) contains four actual-size A4 landscape sheets:

1. Component placement, viewed from above.
2. Component-side jumpers, in red.
3. Solder-side wires, in blue, **already mirrored** for looking at the underside.
4. Both sets of wires together, viewed from above.

Print at **100% / actual size**, without fitting to the page. Check that the
outline measures 160 × 100 mm and ten hole intervals measure 25.4 mm.

## Hole coordinates and wiring

Use a 60-column × 36-row area of the board. Columns are **1–60**, rows
**A–Z, AA–AJ**. The nominal A1 centre is 5.08 mm from the left and top edges.
Commercial board margins vary: choose an actual hole near that position,
mark it A1, and count holes from there. Preserve the relative hole positions;
do not drill holes to match a manufacturer's different edge margin.

All component leads and the seven wire side-change points fall on the
**2.54 mm grid**. Lines between holes show the route of insulated wire, not
additional holes or copper strips. Empty holes are the faint circles on
`Dwgs.User`. A square in the drawing identifies header/IC pin 1; the purchased
board's pads will normally all be round.

The underside sheet flips the board **left/right**, keeping row A at the top.
Column 1 is therefore on the right. Hole names never change between views.
All components are mounted on the top face.

- [Holes.csv](Holes.csv): every component pin, its hole and its electrical net.
- [Wires.csv](Wires.csv): all 91 required pad-to-pad connections, with endpoints,
  side-change holes and routed lengths. Add handling/stripping allowance to
  these lengths. Tick off each connection as it is soldered.
- [Harness.csv](Harness.csv): every external header pin and its destination.
- [Parts.csv](Parts.csv): parts and mounting dimensions for this perfboard.

Use insulated hookup wire except for short, unambiguous adjacent-pad bridges.
Strip only the ends. Intersecting red and blue lines are on opposite faces and
are not joined. Routes belonging to the same net may share a drawn path; the
wire table specifies the actual endpoint connections. A layer-change circle
represents passing an insulated wire through an otherwise empty hole; it is
not a plated via that the perfboard already contains. Top jumpers and component
leads must both be soldered to their appropriate pads on the underside.
Fit low-profile top jumpers before the socket/adapter that covers them.

## Parts and orientation

**Q1–Q3 are the user's Diotec 2N2222A**, with emitter/base/collector identified
as **E/B/C** in the schematic, layout and tables. Diotec numbers these leads
**1 = E, 2 = B, 3 = C**. In the top-view drawing E is on the left and C on the
right; the flat/marked face points toward the bottom edge of the drawing.
The layout uses 2.54 mm lead spacing. Gently spread the leads if the parts are
the 1.27 mm bulk variant. See the
[Diotec package drawing and pinout](https://diotec.com/tl_files/diotec/files/pdf/datasheets/2n2222a.pdf).

**U2 is AM26C32CN**, in a 16-pin DIP socket with 7.62 mm between rows. Its notch
points toward row A. **U3 is SN74LVC125AD**, the SOIC-14 version, on an
[Adafruit 1210 SOIC-14/TSSOP-14 adapter](https://www.adafruit.com/product/1210).
This adapter is 17.78 mm square with **15.24 mm between header rows** and
2.54 mm pin pitch. Mount the **SOIC face upward**, with chip and adapter pin 1
at K50. Use two 1×7 headers, optionally with matching female strips. An ordinary
7.62 mm-wide DIP-14 adapter/socket does **not** fit these allocated holes.
The dimensions and orientation follow
[Adafruit's original adapter CAD](https://github.com/adafruit/Adafruit-SMT-Breakout-PCBs/blob/master/14-pin%20SOIC%2BTSSOP.brd).

The buffer and adapter are purchase items; the user does not own them yet.
Use the specified **LVC** device: it accepts the receiver's 5 V logic while
powered from 3.3 V. A generic 74HC125 at 3.3 V is not a substitute. See
[TI's SN74LVC125A data sheet](https://www.ti.com/lit/ds/symlink/sn74lvc125a.pdf).

R1–R9 are axial resistors with 10.16 mm between holes. C3/C4 are 100 nF
nonpolar ceramic capacitors with 2.54 mm lead spacing, close to their IC supply
connections; C5/C6 are 1 µF nonpolar capacitors with 5.08 mm lead spacing.
R10–R12 reserve positions for encoder termination and are **DNP: leave them
empty**. Their pads may still be used as wire junctions. Do not install links
in those resistor positions. Termination must be chosen for the cable and the
encoder's output-current limit as described in the main README.

## External connections

These are **perfboard header numbers**, not Nucleo connector numbers. All
headers have 2.54 mm pitch. Use the harness table when preparing cables.

| Header | Connection |
| --- | --- |
| J101, 2×8 | Nucleo STEP/DIR/enable outputs and 3.3 V. Odd pins carry signals/power; every even pin is ground. |
| J102, 2×3 | Encoder A/B/Z returns to the Nucleo; each paired even pin is ground. |
| J103, 1×2 | Regulated buck output: pin 1 = +5 V, pin 2 = GND. |
| J104, 1×6 | DM542T: PUL+, PUL−, DIR+, DIR−, ENA+, ENA−, in pin order. Keep the DM542T signal selector at 5 V. |
| J105, 1×10 | M2 Adafruit TMC2209 logic: VDD, GND, DIR, STEP, MS1, MS2, NC, NC, NC, EN. |
| J106, 1×10 | M3 Adafruit TMC2209 logic, same order as J105. |
| J107, 1×8 | Encoder: GND, A+, A−, B+, B−, Z+, Z−, +5 V, matching the motor's numbered encoder connector. |

J101 pin assignments: 1→PE9/CN10.4, 3→PE11/CN10.6,
5→PD14/CN7.16, 7→PD15/CN7.18, 9→PC6/CN7.1,
11→PC7/CN7.11, 13→PF3/CN7.20, 15→3V3/CN8.7.
J102: 1→PB4/CN7.19, 3→PB5/CN7.13, 5→PB6/CN12.17.
Connect the ground conductors to Nucleo ground, common with buck output ground.
The existing system schematic identifies CN11.8 as a Nucleo ground contact.

Both TMC MS1/MS2 pairs are connected to 3.3 V for 1/16 microstepping. Pins
7–9 of J105/J106 have no perfboard connection; leave their harness positions
empty. Cable to the TMC contacts by their labels; a connector's apparent
left/right order changes when viewed from the mating side.

The Nucleo, three motor drivers, buck converter, motor windings and storage
modules remain external. The **24 V distribution and motor currents do not
pass through this perfboard**. Connect those parts as shown in
[the system wiring schematic](../wiring/ProtoMate_Wiring.pdf). In particular,
the Nucleo's 5 V input is supplied separately from the buck; J101 supplies this
interface with the Nucleo's 3.3 V output, not the other way around.

## Checks

[DRC.rpt](DRC.rpt) records KiCad's electrical clearance/connectivity check.
[Verification.txt](Verification.txt) records the comparison of the saved
layout with the existing schematic netlist, hole grid and wire table.
The source schematic remains the electrical reference. This separate assembly
map adds cable headers and is not intended for KiCad's automatic
"Update PCB from Schematic" operation.

The model uses embedded footprints and PTH/via graphics to represent real
component holes and wire passages. Manufacturing silkscreen, missing courtyard
and library-copy checks are disabled for this assembly map; electrical
clearance and unconnected-pad checks remain enabled. Passing those checks does
not verify a soldered assembly. Before fitting ICs or attaching the Nucleo,
check continuity against the wire table and verify the 5 V and 3.3 V supplies
are separate and neither is shorted to ground.

To repeat KiCad's check from the repository root:

```sh
kicad-cli pcb drc --severity-all --exit-code-violations \
  -o /tmp/ProtoMate_Perfboard-DRC.rpt hardware/perfboard/ProtoMate_Perfboard.kicad_pcb
```
