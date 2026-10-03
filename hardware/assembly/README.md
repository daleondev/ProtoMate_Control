# Perfboard assembly

Open [ProtoMate.kicad_pro](../ProtoMate.kicad_pro) in KiCad 10,
then open the PCB editor. This file is a **placement and hand-wiring map for a
purchased 100 × 160 mm individual-pad perfboard**. The tracks represent wires;
there are no Gerber files or custom-board fabrication instructions.

[Assembly.pdf](../exports/Assembly.pdf) contains four actual-size A4 landscape sheets:

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

All component leads and the five additional wire-passage holes fall on the
**2.54 mm grid**. Lines between holes show the route of insulated wire, not
additional holes or copper strips. Empty holes are the faint circles on
`Dwgs.User`. A square in the drawing identifies header/IC pin 1; the purchased
board's pads will normally all be round.

The underside sheet flips the board **left/right**, keeping row A at the top.
Column 1 is therefore on the right. Hole names never change between views.
All components are mounted on the top face.

- [Holes.csv](Holes.csv): every component pin, its hole and its electrical net.
- [Wires.csv](Wires.csv): all 121 required pad-to-pad connections, with endpoints,
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
Fit low-profile top jumpers before the sockets or headers that cover them.

## Parts and orientation

**Q1–Q6 are Diotec 2N2222A**, with emitter/base/collector identified
as **E/B/C** in the schematic, layout and tables. Diotec numbers these leads
**1 = E, 2 = B, 3 = C**. In the top-view drawing E is on the left and C on the
right; the flat/marked face points toward the bottom edge of the drawing.
The layout uses 2.54 mm lead spacing. Gently spread the leads if the parts are
the 1.27 mm bulk variant. See the
[Diotec package drawing and pinout](https://diotec.com/tl_files/diotec/files/pdf/datasheets/2n2222a.pdf).

**U2 is AM26C32CN**, in a 16-pin DIP socket with 7.62 mm between rows. Its notch
points toward row A. **U3 is the user's SN74HC126N**, in a **DIP-14 socket
with 7.62 mm between rows** and 2.54 mm pin pitch. Pin 1 is at **K53**;
pin 14 is at K56. Its notch points toward row A. The previous SOIC adapter
has been removed from this layout. Both DIP chips mount on the top face.

Q1–Q3 serve the DM542T. **Q4–Q6 convert the encoder receiver's 5 V outputs
to 3.3 V for U3**. Each new stage has a 4.7 kΩ base resistor (R20/R23/R26),
a 10 kΩ base-to-emitter resistor (R21/R24/R27), and a 1 kΩ collector pull-up
to 3.3 V (R22/R25/R28). All emitters connect to ground. Collectors connect
to U3 inputs 2, 5 and 9 respectively.

**U2's three differential input pairs have been reversed to cancel the
transistor inversion:** encoder A+/A− go to U2 pins 1/2, B+/B− to 7/6,
and Z+/Z− to 9/10. The external J107 cable pinout stays the same. Follow the
updated wire table even if the previous encoder wiring is already assembled.

U3 pin 14 is **3.3 V**, pin 7 is ground. Its active-high enables **1, 4 and
10 connect to 3.3 V**; unused enable 13 and unused input 12 connect to ground,
and output 11 is unconnected. **No HC126 input connects directly to 5 V.**
The resulting A/B/Z signals retain their original polarity, so the existing
timer and rising-edge index configuration stays valid. See
[TI's SN74HC126 data sheet](https://www.ti.com/lit/ds/symlink/sn74hc126.pdf).

Keep the transistor collector connections short. Verify edge shape and
turn-off delay at the intended encoder speed with an oscilloscope; saturated
transistor storage delay is not established by the static wiring checks.
No 4N35 optocouplers are needed for this circuit with a common ground.

R1–R9 and R20–R28 are axial resistors with 10.16 mm between holes. C3/C4 are 100 nF
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
| J108, 2×3 | Nucleo reference inputs: 1 = M1_REF, 3 = M2_REF, 5 = M3_REF; 2/4/6 = GND. |
| J109/J110/J111, each 1×2 | M1/M2/M3 reference switch respectively: pin 1 = REF to NC contact, pin 2 = GND to COM contact. |

J101 pin assignments: 1→PE9/CN10.4, 3→PE11/CN10.6,
5→PD14/CN7.16, 7→PD15/CN7.18, 9→PC6/CN7.1,
11→PC7/CN7.11, 13→PF3/CN7.20, 15→3V3/CN8.7.
J102: 1→PB4/CN7.19, 3→PB5/CN7.13, 5→PB6/CN12.17.
J108: 1→PE7/CN10.20, 3→PE8/CN10.18, 5→PE10/CN10.24;
join its ground returns (pins 2/4/6) at Nucleo CN10.22.
Connect the ground conductors to Nucleo ground, common with buck output ground.
The existing system schematic identifies CN11.8 as a Nucleo ground contact.

Both TMC MS1/MS2 pairs are connected to 3.3 V for 1/16 microstepping. Pins
7–9 of J105/J106 have no perfboard connection; leave their harness positions
empty. Cable to the TMC contacts by their labels; a connector's apparent
left/right order changes when viewed from the mating side.

### Reference-switch cables

The added headers occupy the previously empty top-left area. Existing component
locations and routes are preserved. J108 pin 1 is **B4**, J109 pin 1 is **B10**,
J110 pin 1 is **B14**, and J111 pin 1 is **B18**. Each switch header's adjacent
pin 2 is ground. Add wires **W113–W121**; the three signals use the underside,
while some ground links use insulated top jumpers. No new wire-passage holes
or active components are needed.

SW1/SW2/SW3 are the external, unpowered Creality mechanical switches. The
schematic identifies **COM, NC and NO by function**, not by PCB connector
position. With each switch disconnected, find the pair that has continuity
when released and opens when pressed. Connect that pair to its REF/GND cable;
leave the remaining contact unused. Do not apply power based on `V/G/S`
silkscreen labels. Only the unpopulated dry-contact version is represented.

The Nucleo supplies the signal bias through its configured internal **3.3 V
pull-ups**: low = released, high = pressed or unplugged. The perfboard adds no
pull-up, debounce or filter components. Route each signal with its ground
return away from motor wiring. Before motion use, validate noise immunity and
contact debounce with the actual cables; filtering and homing/stop logic are
not implemented by this wiring addition.

The Nucleo, three motor drivers, buck converter, motor windings, reference switches and storage
modules remain external. The **24 V distribution and motor currents do not
pass through this perfboard**. Connect those parts as shown in
[the system wiring schematic](../exports/Wiring.pdf). In particular,
the Nucleo's 5 V input is supplied separately from the buck; J101 supplies this
interface with the Nucleo's 3.3 V output, not the other way around.

## Checks

[DRC.rpt](../exports/DRC.rpt) records KiCad's electrical clearance/connectivity
and schematic-parity checks. [Verification.txt](../exports/Verification.txt)
records checks of the saved layout's symbol links, nets, hole grid, wire table
and harness destinations. The schematic includes J101–J111 and is the
electrical reference for this layout. Use **Update PCB from Schematic (F8)**
as described in the [project guide](../README.md).

The model uses project-local footprints and PTH/via graphics to represent real
component holes and wire passages. Manufacturing silkscreen and missing-courtyard
checks are waived for this assembly map; electrical clearance, unconnected-pad,
schematic-parity and library-consistency checks remain enabled. Passing those checks does
not verify a soldered assembly. Before fitting ICs or attaching the Nucleo,
check continuity against the wire table and verify the 5 V and 3.3 V supplies
are separate and neither is shorted to ground.

To repeat KiCad's check from the repository root:

```sh
python3 hardware/tools/refresh.py --check-only
```
