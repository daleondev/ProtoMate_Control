# Smartwin–GIGA display adapters

The **two-layer display adapter (revision C)** and **four-layer GIGA adapter
(revision B)** connect the Smartwin **SMMH0701024600S-C20** to the Arduino
**GIGA R1 WiFi**, preserving the firmware's display pin assignments.
Open either project in KiCad 10; both use `libraries/` and require no downloaded
symbol or footprint library. Keep this directory structure when moving them.

Release files:

- [Display adapter: fabrication and assembly ZIP](releases/display-adapter-rev-C-pcbway.zip)
- [GIGA adapter: fabrication and assembly ZIP](releases/giga-adapter-rev-B-pcbway.zip)
- [Both KiCad projects, libraries and models](releases/smartwin-giga-rev-C-source.zip)
- [Nominal GIGA mating assembly preview](assembly/giga-mated-rev-B.png)

Each PCBWay ZIP contains `PCBWAY_ORDER.txt`, separate SMT/full BOMs and the
matching placement file. `releases/SHA256SUMS` identifies the final archives.

| Project | Board | Assembly |
|---|---|---|
| `display-adapter/display-adapter.kicad_pro` | Rev C, **2 layers**, 55 × 35 × 1.0 mm | SMT on both sides; U1 converter hand soldered |
| `giga-adapter/giga-adapter.kicad_pro` | Rev B, **4 layers**, 111 × 53.34 × 1.6 mm maximum, central power tab | SMT on top; J2–J10 and U8 hand soldered |

The display adapter carries a **DF23C-50DP-0.5V(92) plug on its underside**.
It mates with the existing display connector; do not remove the display connector
or solder the adapter directly to the display flex. The GIGA adapter plugs into
the existing headers, with stacking sockets preserving access to every collected
contact. USB, SWD, buttons, LEDs and stacking contacts remain accessible. The inward power tab occupies the upper central area; the lower service opening remains clear.

**Physical operation has not been tested.** Fabrication verification and first-board
electrical qualification are separate. The machine-readable release status and
verification reports in `manufacturing/` identify which checks have passed.
Do not manufacture a working-directory draft without its matching release ZIP.

## Connection and power

```text
Regulated 24 V -> GIGA shield J10 -> fuse / TVS / TPS26600 protection
                                     |                    |
                               TSR 1-24120          0.8 A branch fuse
                                   12 V                   |
                              blocking diode         80-way ribbon
                                     |                    |
                                  GIGA VIN       display TSR 2-2433
                                                          |
                                                 3.3 V -> Smartwin
                                                          |
                                   separate 3.3 V reference return to buffers
```

Connect the existing **regulated 24 V supply to shield J10: pin 1 = +24 V,
pin 2 = GND**. Allow at least 2 A supply capacity for this assembly; the input
protection limits combined current to approximately 1.49 A. The recommended
supply range is 24 V ±10%. Keep grounds common with the robot. This input powers
both GIGA and the display; the display adapter has no separate power terminal.

- **Shield U8: TRACO TSR 1-24120**, 24 V to 12 V, rated 1 A output. D3 feeds
  VIN through a Schottky diode, so actual VIN is slightly below 12 V. VIN reaches
  both corresponding GIGA header contacts, J5.8 and J9.24.
- **Display U1: TRACO TSR 2-2433**, 24 V to 3.3 V, rated 2 A output. Its local
  10 µF/50 V and 100 nF/50 V input capacitors suppress cable-side switching
  current. Four 47 µF output capacitors and local 10 µF/100 nF bypass capacitors
  support the panel load; nominal external output capacitance stays below the
  module's 900 µF limit. Do not exchange the two converter types.
- **USB-C may remain connected for CDC CLI/logging.** GIGA retains its onboard
  USB/VIN power arrangement. D3 blocks reverse feed from VIN into U8 when only
  USB is present. USB alone runs GIGA, but does not power the display or the
  external 24 V rail. Do not connect another source to VIN simultaneously.
- **GIGA_3V3 and PANEL_3V3 remain separate.** Three FFC contacts return the panel
  rail to the panel sides of the AXC/TCA interface devices. This return carries
  interface current, while the backlight's 3.3 V current stays on the display
  board. All grounds are common.

Shield F1 is a **2 A/63 V time-lag fuse** and D1 a bidirectional **SMBJ33CA** TVS.
U7 **TPS26600PWPR** provides reverse-polarity/reverse-current protection,
undervoltage and overvoltage cutoff, controlled inrush and current limiting.
The dividers set nominal startup at **18.2 V** and overvoltage cutoff at
**28.1 V**. Including comparator and resistor tolerances, the upper cutoff is
29.47 V, below the converters' 36 V input rating. C16 gives an approximately
9.8 ms protected-24 V ramp; MODE selects automatic retry after overload.
The TVS handles short transients, while the eFuse disconnects excessive input
voltage. These component ratings are not a tested industrial surge rating.

**U7 RTN and its exposed pad must not be connected to GND.** They use a separate
thermal copper island and four thermal vias. Pin 9 alone is the system-ground
connection. D2 clamps negative excursions on the protected output. F2 is a
**0.8 A/63 V time-lag fuse** for the ribbon branch. Two parallel contacts carry
24 V; even at 2 A converter output, 18 V input and an 80% efficiency allowance,
each carries approximately 0.23 A, below the connector's 0.5 A/contact rating.
The specified cable copper and contacts have under 0.10 V allocated drop in
that conservative load case. Power is disconnected before inserting/removing
the ribbon; these connectors are not hot-plug power connectors.

The display qualification budget is **1.5 A at 3.3 V**: 1.05 A typical backlight,
0.25 A maximum TFT, and 0.20 A for touch/interface current and margin. The
backlight maximum is unspecified; 1.5 A is not a guaranteed panel maximum.
All four backlight contacts are connected; at the documented typical current
each carries approximately 0.263 A, below the DF23's 0.3 A/contact rating.

The calculated panel-voltage budget uses converter accuracy, line/load
regulation, ±0.02%/K temperature coefficient over a 0–70°C calculation range,
a 50 mV negative ripple allowance, 40 mΩ of PCB supply/return resistance and
30 mV of contact drop. At 1.5 A this leaves approximately **3.015 V**, above the
panel's **3.0 V minimum**, with limited worst-case margin. This is a conservative
allocation, not measured performance; verify it at CN1 under full brightness
and the actual enclosure temperature. Module derating, ripple, current sharing
and the panel's **1–20 ms supply rise time** remain first-board checks.
`manufacturing/verification/power-budget.json` records the calculations and
assumptions. Do not exceed the panel's 3.6 V absolute supply limit.

## Interfaces and startup

- U1–U4 are SN74AXC8T245PWR, with VCCA = GIGA and VCCB = panel. **DIR1 is HIGH,
  DIR2 is LOW**, which selects A→B for all eight channels. Their `/OE` inputs are
  LOW. Each MCU input has a 47 kΩ pull-down. The panel's PWM, standby and reset
  lines also have 47 kΩ pull-downs, so disabled buffers leave the backlight off
  and resets asserted. The AXC devices provide partial-power-down isolation.
- U5 is SN74AXC1T45DBVR, A = panel interrupt, B = GIGA input, DIR = panel rail.
  Both interrupt sides have defined pull-ups. The interrupt is active LOW.
- U6 is TCA9517ADGKR: **A side faces the panel**, B side faces the GIGA, EN is tied
  to GIGA 3.3 V. Panel SDA/SCL use 4.7 kΩ pull-ups. GIGA already has 5.1 kΩ
  pull-ups on I2C4 (R26/R27), so the adapter does not duplicate them. The B-side
  LOW offset is compatible with the STM32 input threshold. Use the existing
  100 kHz I2C4 configuration and touch address 0x55.
- All 32 outgoing channels have 33 Ω source-series resistors after the buffers.
  These are initial damping values, not a claim of an impedance-matched channel.
  PCLK and ground test points are provided at both ends. All resistor pads are
  accessible for probing individual RGB/control channels.
- The existing panel backlight driver is retained: 1 kHz PWM, including steady
  HIGH for full brightness. No external LED-current driver is required.
- The GIGA firmware's reset/standby sequencing still applies. Supply isolation
  does not automatically reinitialize the panel when its power returns; use the
  existing display initialization commands after a separate panel power cycle.

## Cable and numbering

The exact procurement specification is `cable-specification.pdf`; the complete
80-conductor map is `cable-conductors.csv`. `connections.csv` adds the MCU pin,
Arduino label, GIGA header, adapter header, both FFC contacts and display contact
for each of the 35 signals.

Use **80 conductors, 0.50 mm pitch, 0.30 mm mating thickness, straight-sided FFC,
Type A (exposed contacts on the same side), at most 100 mm long**. Both sockets are
Hirose FH28H-80S-0.5SH(05). Follow the drawing's contact-face orientation, not the
colour of an arbitrary cable stiffener. A Type B/opposite-contact cable is not
an interchangeable substitute.

The facing connectors reverse numbering: **GIGA J1 pin n connects to display J2
pin 81−n**. Forty conductors are ground, 35 carry signals, two carry protected 24 V, and
three return the panel supply reference. PCLK has ground on both sides.

| Rail | GIGA J1 contacts | Display J2 contacts |
|---|---|---|
| Protected, branch-fused 24 V | 1, 3 | 80, 78 |
| PANEL_3V3 reference return | 5, 7, 9 | 76, 74, 72 |

Use the **display-C / GIGA-B pair**. Its powered cable assignment is incompatible
with the earlier separately powered adapters: do not mix revisions. The cable
must be rated at least 60 V and 0.5 A per conductor, with copper cross-section
at least 0.0105 mm²; a mechanically fitting cable alone is insufficient.

Viewed from the display adapter's top, J1 pin 1 is at the lower left of its pad
array, pin 2 above it, and pin 49 at the lower right. J1 itself is mounted on the
bottom. Use the assembly bottom view and Smartwin's rear-view Detail A together;
do not infer pin numbering by mirroring a top-side footprint screenshot.

## Mechanics and assembly

The GIGA geometry comes from Arduino's official Altium CAD, not a generic Mega
shield outline. The inner display header's **mating contact columns are 2.54 mm
apart**, although its SMT solder-pad columns are 6.1 mm apart. The footprints use
the contact-entry centres, verified against the CAD's entry holes.

Mount the carrier **13.0 mm above the GIGA top surface** (adapter underside).
J2–J8 use Samtec SSQ `-03` 10 mm tails. J9 uses **SSQ-112-04-G-D** with 14.83 mm
tails to reach the lower inner GIGA header. Those longer tails enter the existing
clearance holes below that header. Do not substitute equal-tail-length headers
or force the carrier down against the shorter inner socket.

The two lower mounting positions accept M2 support hardware; the two upper holes
are alignment clearances, without screw heads beside the headers. Use insulating
13 mm supports (for example a 12 mm spacer plus a 1 mm washer), and support the
FFC end of the carrier in the enclosure. The CAD fit model documents the nominal
stack; manufacturing tolerances and actual connector insertion are checked at
assembly. Do not use the connector pins as the board's structural support.

The display board has four M2 mounting holes. Attach it to a frame or bracket
with insulating standoffs, leaving the bottom connector's mating envelope clear.
Add cable clamping to that frame near each FFC socket. The ribbon, power leads
and adapter must not hang from the display flex or its DF23 connector. Keep the
FFC short and flat; do not sharply crease it at either stiffener.

PCBWay assembles every SMT part, including the bottom-side DF23. U1 on the display
adapter, and J2–J10 plus U8 on the GIGA adapter, are **HAND** parts in the full BOM and absent
from the SMT BOM/centroid file. Solder stacking headers with a mechanical jig or
an unpowered GIGA used only as an alignment fixture; avoid heating the board's
existing sockets unnecessarily. Fit shield J10 with its wire opening
facing left into the central opening. Probe pads, fiducials and mounting holes are PCB
features, not purchased parts.

After soldering display U1, **clip its three leads to at most 0.8 mm below the PCB**, without
cutting into the solder joints. The local assembly model includes this trim.
This keeps the leads above the display flex's nominal 1.5 mm mating plane.

The local STEP models for Hirose connectors, terminal, converter/eFuse envelopes and stacking sockets are
dimensioned mechanical envelopes derived from manufacturer drawings. They
simplify latch/contact details and are labelled accordingly; they are not
manufacturer production-tooling models. Standard IC/passive models and the
official Arduino assembly are retained locally as well.

## Timing and layout

The firmware's 51.2 MHz pixel clock has a 19.53125 ns period and a 9.765625 ns
half-period. Smartwin requires 5 ns setup and hold. With data launched on the
opposite edge, the AXC8's 0.5–4.0 ns propagation range gives a conservative
3.5 ns clock/data mismatch allowance across devices. The adapter's routed RGB
length mismatch contributes up to 0.406 ns using a 7 ps/mm propagation estimate.
ST's DS12930 Table 105 additionally allows 0.5 ns half-cycle error and 0.5 ns
maximum data-valid delay. Under that table's characterization conditions, the
remaining calculated setup/hold margins are only **0.238 ns / 0.360 ns**, before
Arduino PCB skew, cable/connector effects, settling and jitter. Those conditions
include the specified output speed, load and IO compensation; this hardware
task does not change firmware to establish them.

This is **not complete worst-case timing closure**. The routed-length report
and `timing-budget.json` expose the calculation and omitted terms. The boards
are prepared for prototype fabrication; reliable operation across voltage and
temperature requires the first-board timing qualification below.

Keep the 100 mm cable limit. At first-board qualification measure PCLK and data
at the panel connector, confirm the configured sampling phase, and adjust series
resistors only from measurements. The panel's stated 40–60% duty-cycle limit and
the MCU's own output timing are part of this check. A clean static image alone
does not prove timing margin over temperature and supply variation.

The display adapter routes all signals on **F.Cu and B.Cu**, with ground pours on
both sides and 41 ground vias tying the returns together. A ground rail connects
the FFC ground contacts. Staggered Hirose escapes use **0.55 mm copper / 0.25 mm
drilled vias**, retaining 0.15 mm annular rings and 0.15 mm track/clearance rules.
The larger power and ground vias remain 0.65/0.30 mm. Bulk capacitors are below
the connector fan-out; the backlight feed uses dedicated broad traces. PCLK has
a deliberate length adjustment to balance setup/hold against the GIGA routing.
These are connected ground pours, not an uninterrupted internal reference plane;
the first-board signal-integrity checks remain necessary. The geometric ground
review is in `manufacturing/verification/display-adapter-layout-review.json`.

The GIGA adapter uses an **In1.Cu ground reference plane**, with signals
on F.Cu, In2.Cu and B.Cu and minimum 0.65/0.30 mm vias. Both boards use minimum
0.15 mm tracks and clearances. Vias are tented on both outer sides,
including beneath the Hirose bodies. No open via is used in a solder pad.
Global fiducials are provided on both populated sides of the display adapter
and on the GIGA adapter's populated top side.
Fine-pitch mask webs are at least 0.13 mm at the FFC and I2C buffer pads. The FFC
paste windows follow Hirose's 0.25 × 1.00 mm recommendation.

J8 uses 1.55 mm pads with 1.05 mm holes, leaving a 0.25 mm annular ring and space
for routing between contacts. Other stacking headers retain 1.8 mm pads.

## Fabrication and assembly order

Use the **display revision-C ZIP with two layers** and the **GIGA revision-B ZIP
with four layers**. Select FR-4, 1 oz copper on every copper layer, ENIG, green
solder mask, white silkscreen, IPC Class 2 and
100% electrical test. Finished thickness is **1.0 mm display / 1.6 mm GIGA**.
The routed outline is authoritative. Do not mirror bottom Gerbers. Request
PCBWay's assembly panelization/rails; do not alter the individual board outline.

The package includes copper/mask/silk/paste/outline Gerbers, separate PTH/NPTH
drills, electrical-test netlist, full and SMT-only BOMs, centroid CSV, schematics,
fabrication/assembly drawings and STEP. All coordinates are millimetres; the
drill/place origin is X=0 at the lower board edge, positive Y upward. Bottom-side
centroid X is not negated; rotation follows KiCad's native placement convention.
Use the assembly drawings to resolve connector orientation during CAM review.

Hirose specifies a 0.15 mm stencil for the FH28 land pattern. Request a **stepped
stencil: 0.15 mm at the FFC, 0.10–0.12 mm at 0402/TSSOP parts**, using the supplied
paste apertures. PCBWay may propose an equivalent qualified paste process; do not
silently enlarge the fine-pitch apertures or omit their mask webs. Confirm exact
Hirose suffixes and Samtec tail options when sourcing; no connector substitutions.

Current capability references: [PCBWay fabrication](https://www.pcbway.com/capabilities.html)
and [assembly requirements](https://www.pcbway.com/assembly-faq.html). Uploading
files does not place an order; no order has been placed by this work.

## First-board acceptance

1. With 24 V and USB disconnected and no panel fitted, check for shorts, all connector
   contacts and all 80 cable conductors. Confirm that GIGA_3V3 and PANEL_3V3 are
   separate, and all specified grounds are connected. Verify cable orientation.
2. Apply current-limited 24 V with GIGA/panel initially unplugged. Check U8's
   12 V, VIN after D3, and the display 3.3 V output before attaching loads.
   Verify UV/OV cutoffs and current limiting using a bench supply. Then test
   USB-only operation: no 24 V or panel-3.3 V backfeed, backlight off, resets LOW.
3. Attach the unpowered panel, power normally and run `display bars`. Check colour
   order, all active pixels and both framebuffer slots. Exercise brightness from
   zero to full and `display touch`, including touch reset and interrupt.
4. Measure panel supply at full brightness, input/backlight current, connector
   and protection-component temperatures, PCLK frequency/duty/phase and data
   setup/hold at the panel. Qualify the 33 Ω values and the full 100 mm cable.
5. Test 24 V first, USB first, removal of either supply and explicit display
   reinitialization after power recovery. Repeat under graphics/ICC/logging load.

## Reproducibility

`tools/build.py` regenerates libraries/schematics and **placement-only boards in
`build/`**. It does not overwrite reviewed project routing. `prepare_routes.py`
adds the initial power and ground escapes. Routing tools are generation aids;
the checked-in project boards are the authoritative layouts.

`tools/audit.py` independently compares the exported schematic netlist, PCB pad
nets, manufacturer pin tables, all cable conductors and the CubeMX pin assignment.
`tools/export_release.py` requires clean ERC/DRC and checks BOM/placement agreement.
`tools/inspect_gerbers.py` parses the exported files with Gerbonara, independent of
KiCad, and checks exported pad/drill coordinates and layer geometry. Release
archives are made only after these checks and visual/mechanical review.

`tools/design.py` defines each board's revision and layer/via rules separately;
the source bundle revision is C. `tools/review_layout.py` uses Shapely to measure
the display board's filled ground geometry. It reports geometry only, not a
field-solver or physical timing result. To refresh the display release, run
`export_release.py display-adapter`, `audit.py`, `routing_report.py`,
`review_layout.py`, and `inspect_gerbers.py display-adapter`; regenerate its
fabrication drawing with `drawings.fabrication('display-adapter')`, review the
exports, then run `package_release.py --reviewed --boards display-adapter`.
Use the release status to record the actual reviews before packaging.

Source documents and attribution are listed in `references/SOURCES.md`.
