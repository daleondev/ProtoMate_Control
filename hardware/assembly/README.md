# Perfboard assembly

Open [ProtoMate.kicad_pro](../ProtoMate.kicad_pro) in KiCad 10,
then open the PCB editor. This file is a **placement and hand-wiring map for a
purchased individual-pad perfboard with one continuous 39 × 48 hole grid**.
The tracks represent wires; there are no Gerber files or custom-board
fabrication instructions.

[Assembly.pdf](../exports/Assembly.pdf) contains four actual-size A4 landscape sheets:

1. Component placement, viewed from above.
2. Component-side jumpers, in red.
3. Solder-side wires, in blue, **already mirrored** for looking at the underside.
4. Both sets of wires together, viewed from above.

Print at **100% / actual size**, without fitting to the page. Check that the
nominal outline measures 99.06 × 121.92 mm and ten hole intervals measure 25.4 mm.

## Hole coordinates and wiring

Use the entire **39-column × 48-row grid** as one board. Columns are **1–39**,
rows **A–Z, AA–AV**, giving 1,872 holes. A1 is the upper-left hole in the top
view. The distance between outer hole centres is **96.52 × 119.38 mm**.
The drawing puts the outline half a pitch (1.27 mm) beyond the outer hole
centres; assembly coordinates are defined by the holes, independently of the
physical edge margins. The grid continues uniformly across the whole assembly.

All component leads and the additional wire-passage holes fall on the
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
- [Harness.csv](Harness.csv): every header pin and its cable or direct module contact.
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
with 7.62 mm between rows** and 2.54 mm pin pitch. Pin 1 is at **S33**;
pin 14 is at S36. Its notch points toward row A. Both DIP chips mount on the top face.

Q1–Q3 serve the DM542T. **Q4–Q6 convert the encoder receiver's 5 V outputs
to 3.3 V for U3**. Each stage has a 4.7 kΩ base resistor (R20/R23/R26),
a 10 kΩ base-to-emitter resistor (R21/R24/R27), and a 1 kΩ collector pull-up
to 3.3 V (R22/R25/R28). All emitters connect to ground. Collectors connect
to U3 inputs 2, 5 and 9 respectively.

**U2's three differential input pairs are reversed to cancel the
transistor inversion:** encoder A+/A− go to U2 pins 1/2, B+/B− to 7/6,
and Z+/Z− to 9/10. Use the J107 cable pinout and wire table below.

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
contacts land on the 2.54 mm grid; J103's pairs span two intervals (5.08 mm).
Use the harness table for cables and direct module contacts.

| Header | Connection |
| --- | --- |
| J101, 2×8 | Nucleo STEP/DIR/enable outputs and 3.3 V. Odd pins carry signals/power; every even pin is ground. |
| J102, 2×3 | Encoder A/B/Z returns to the Nucleo; each paired even pin is ground. |
| J103, four module pins | Buck: 1 = OUT+ / 5 V, 2 = OUT− / GND; 3/4 = isolated VIN support pads, no perfboard wires. |
| J104, 1×6 | DM542T: PUL+, PUL−, DIR+, DIR−, ENA+, ENA−, in pin order. Keep the DM542T signal selector at 5 V. |
| J105, 1×10 | Direct soldered JP4 header of M2 / A3: VDD, GND, DIR, STEP, MS1, MS2, DIAG, INDEX, UART, EN. |
| J106, 1×10 | Direct soldered JP4 header of M3 / A4, same order as J105. |
| J107, 1×8 | Encoder: GND, A+, A−, B+, B−, Z+, Z−, +5 V, matching the motor's numbered encoder connector. |
| J108, 2×3 | Nucleo reference inputs: 1 = M1_REF, 3 = M2_REF, 5 = M3_REF; 2/4/6 = GND. |
| J109/J110/J111, each 1×2 | M1/M2/M3 reference switch respectively: pin 1 = REF to NC contact, pin 2 = GND to COM contact. |

J101 pin assignments: 1→PA0/CN10.29, 3→PE12/CN10.26,
5→PB10/CN10.32, 7→PE13/CN10.10, 9→PB11/CN10.34,
11→PE14/CN10.8, 13→PE15/CN10.30, 15→3V3/CN8.7.
All STEP pins use TIM2 output compare / AF1 (channels 1, 3, 4).
PA0 uses SB75 ON; keep the user button on PC13 (SB58 OFF).
PE9/PE11 and PC6/PC7 are reserved for future encoder inputs and have no cables fitted.
Configure the onboard STLINK-V3 MCO to **HSE/5 = 5 MHz**, matching CubeMX's
HSE bypass clock. See the [clock setup procedure](../../docs/stepper-hardware-test.md#clock-source-configuration).
J102: 1→PB4/CN7.19, 3→PB5/CN7.13, 5→PB6/CN12.17.
J108: 1→PE7/CN10.20, 3→PE8/CN10.18, 5→PE10/CN10.24;
join its ground returns (pins 2/4/6) at Nucleo CN10.22.
Connect the ground conductors to Nucleo ground, common with buck output ground.
The existing system schematic identifies CN11.8 as a Nucleo ground contact.

Both TMC MS1/MS2 pairs are connected to 3.3 V for 1/16 microstepping. Pins
7–9 of J105/J106 have isolated landing pads: fit the complete ten-pin header,
but add no perfboard wires to DIAG, INDEX or UART. No logic cable is required.

### Directly mounted TMC2209 modules

Fit each module component-side up, with its **JP4 male header pointing down**
through the perfboard. J105 pin 1 (VDD) is **J13** and its pin 10 (EN) is
**J22**. J106 pin 1 is **J27** and its pin 10 is **J36**. Pin numbers increase
left to right in the top view. Both modules sit side by side along the top edge,
with their screw terminals facing toward row A.

The footprints include the **26.67 × 24.13 mm** module bodies and the terminal
blocks, measured from [Adafruit's official PCB drawing](https://github.com/adafruit/Adafruit-TMC2209-Breakout-PCB).
The four larger circles inside each outline are mounting holes **in the
Adafruit module**, not new holes to drill in the perfboard. Their spacing does
not match the perfboard grid. Use insulating supports under the terminal side
so tightening screws does not bend the header or its solder joints.

Use long-tail headers and supports to leave **at least 6 mm between the module
underside and the perfboard top**. Check the actual terminal solder tails clear
all jumper insulation, solder joints and pads. Fit low-profile insulated wires
first, then supports and modules. Keep the driver chip and current-adjustment
potentiometer accessible from above; allow space above for cooling.

Retain each driver's JP1 screw terminals. Connect its 24 V pair, motor cable
and local bulk capacitor directly there, as shown in the system schematic.
Do not route winding currents or 24 V through the ten-pin control header.

### Buck converter mounting

The **63 × 27 mm PCB** sits at the lower left, with its barrel jack pointing
outward at the left edge and USB connector toward the right. The body follows
the supplied [BuckConverter.step](../../docs/BuckConverter.step); mounting pins
use a nominal **5.08 mm pair pitch** and **50.8 mm pair separation**,
based on the model's terminal blocks. R6 lies horizontally above the buck.

Remove both screw-terminal blocks and fit four long-tail header pins, keeping
the module component-side up and at least 6 mm above the perfboard. The
project's buck 3D model shows this arrangement, retaining the barrel jack and
USB socket. With the barrel jack facing left, the output positive contact is
above the negative contact, following the
[XY-3606 terminal layout](https://roboticsdna.in/product/24v-12v-to-5v-5a-power-module-dc-dc-xy-3606-power-converter/).
Use the module's **OUT+ and OUT−** contacts for the 5 V and ground connections.

| J103 pin | Hole, top view | Connection |
| --- | --- | --- |
| 1 | AL23 | Buck OUT+ → perfboard 5 V |
| 2 | AN23 | Buck OUT− → perfboard GND |
| 3 | AN3 | VIN-side mechanical solder support; no perfboard wire |
| 4 | AL3 | VIN-side mechanical solder support; no perfboard wire |

Feed **24 V through the existing barrel jack**. Solder the two input header
pins to their isolated islands for support only. They still carry input
ground and 24 V through the module; the no-connect marks mean no added
perfboard connection. The layout reserves a wire-free area on **both faces**
around these pads. Keep that area free of jumper wires and solder bridges.
The Nucleo 5 V supply remains a separate branch from the buck output;
J101 carries the Nucleo's 3.3 V output.

### Reference-switch cables

The reference headers occupy the lower-right area. J108 pin 1 is **AM28**,
J109 pin 1 is **AM34**, J110 pin 1 is **AP34**, and J111 pin 1 is **AS34**.
Each switch header's adjacent pin 2 is ground. Wires **W113–W121** connect them;
use the current wire table
for sides and wire-passage holes.

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

The Nucleo, DM542T, motors, reference switches and storage modules remain
external. U1/A3/A4 sit directly on J103/J105/J106; **24 V distribution and motor
currents do not pass through the perfboard wiring**. The buck input support
islands are energized through the module but have no hand wires.
Connect those parts as shown in
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
