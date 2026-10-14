# Smartwin–GIGA display adapters — revision A

Two four-layer boards connect the Smartwin **SMMH0701024600S-C20** to the Arduino
**GIGA R1 WiFi**, preserving the firmware's existing display pin assignments.
Open either project in KiCad 10; both use `libraries/` and require no downloaded
symbol or footprint library. Keep this directory structure when moving them.

Release files:

- [Display adapter: fabrication and assembly ZIP](releases/display-adapter-rev-A-pcbway.zip)
- [GIGA adapter: fabrication and assembly ZIP](releases/giga-adapter-rev-A-pcbway.zip)
- [Both KiCad projects, libraries and models](releases/smartwin-giga-rev-A-source.zip)
- [Nominal GIGA mating assembly preview](assembly/giga-mated-rev-A.png)

Each PCBWay ZIP contains `PCBWAY_ORDER.txt`, separate SMT/full BOMs and the
matching placement file. `releases/SHA256SUMS` identifies the final archives.

| Project | Board | Assembly |
|---|---|---|
| `display-adapter/display-adapter.kicad_pro` | 55 × 35 × 1.0 mm | SMT on both sides; J3 hand soldered |
| `giga-adapter/giga-adapter.kicad_pro` | 111 × 53.34 × 1.6 mm maximum, C-shaped | SMT on top; J2–J9 hand soldered |

The display adapter carries a **DF23C-50DP-0.5V(92) plug on its underside**.
It mates with the existing display connector; do not remove the display connector
or solder the adapter directly to the display flex. The GIGA adapter plugs into
the existing headers, with stacking sockets preserving access to every collected
contact. USB, SWD, buttons, LEDs and the central board area remain accessible.

**Physical operation has not been tested.** Fabrication verification and first-board
electrical qualification are separate. The machine-readable release status and
verification reports in `manufacturing/` identify which checks have passed.
Do not manufacture a working-directory draft without its matching release ZIP.

## Connection and power

```text
GIGA → stacking adapter J1 ⇄ 80-way FFC ⇄ display adapter J2
                                              │
External regulated 3.3 V → display J3 → protection → J1 → Smartwin
```

Use **3.30 V ±1% at J3 under load**, with at least **2 A continuous capability**.
J3 pin 1 is positive and pin 2 is ground. This is a 3.3 V input, not a 5 V input.
The GIGA's 3.3 V output must not power the panel or be connected to this supply.
Ground is common. The panel supply crosses the cable only to power the display
sides of the interface ICs; backlight current stays on the display adapter.

F1 is a 2.5 A fast fuse. Q1/Q2 are parallel reverse-polarity PMOS devices, with
their drains toward the input and sources toward the protected panel rail.
This protects against reversed input and limits fault energy; it does not regulate
the voltage or protect against connecting a sustained 5 V supply.
Four 47 µF capacitors provide bulk decoupling; 10 µF and 100 nF capacitors provide
local TFT/touch decoupling. Each interface IC has local bypass capacitors.

The design current budget is **1.5 A**: 1.05 A typical backlight, 0.25 A maximum TFT,
and 0.20 A reserved for touch, interface switching and margin. Smartwin does not
specify a maximum backlight current; this is a qualification budget, not a claim
that 1.5 A is the panel's guaranteed maximum. The four DF23 backlight contacts are
all connected; at the documented typical current each carries approximately
0.263 A, below the connector's 0.3 A/contact rating. First-board testing must
confirm current sharing, voltage and temperature at full brightness.

Conservative voltage-drop allowance at 1.5 A:

| Element | Allowance | Drop |
|---|---:|---:|
| Fuse, including operating-temperature allowance | 40 mΩ | 60 mV |
| Parallel PMOS pair, twice the 25°C maximum resistance | 45 mΩ | 67.5 mV |
| Input terminal contact | 10 mΩ | 15 mV |
| PCB supply/return copper and vias | 20 mΩ | 30 mV |
| DF23 supply and return contacts | allocated | 30 mV |
| Total | | **202.5 mV** |

At a 3.267 V minimum input this leaves approximately **3.064 V** at the panel,
above its 3.0 V minimum. Fuse/copper/hot-device allowances must be checked on the
first board; external cable loss is excluded because the input specification is
at J3. The panel also specifies a 1–20 ms supply rise time. Verify the chosen
regulated supply and wiring meet that requirement; the PMOS stage is not a
controlled soft-start circuit. Do not compensate for excessive loss by exceeding
the panel's 3.6 V absolute supply limit.

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
pin 81−n**. Forty-one conductors are ground, 35 carry signals, and four return the
panel supply reference. PCLK has ground immediately on both sides. GIGA contacts
3/5/7/9 carry PANEL_3V3 and connect to display contacts 78/76/74/72.

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

PCBWay assembles every SMT part, including the bottom-side DF23. J3 on the display
adapter and J2–J9 on the GIGA adapter are **HAND** parts in the full BOM and absent
from the SMT BOM/centroid file. Solder stacking headers with a mechanical jig or
an unpowered GIGA used only as an alignment fixture; avoid heating the board's
existing sockets unnecessarily. Fit the power terminal with its wire opening
facing the lower board edge. Probe pads, fiducials and mounting holes are PCB
features, not purchased parts.

After soldering J3, **clip its two leads to at most 0.8 mm below the PCB**, without
cutting into the solder joints. The local assembly model includes this trim.
This keeps the leads above the display flex's nominal 1.5 mm mating plane.

The local STEP models for Hirose connectors, terminal and stacking sockets are
dimensioned mechanical envelopes derived from manufacturer drawings. They
simplify latch/contact details and are labelled accordingly; they are not
manufacturer production-tooling models. Standard IC/passive models and the
official Arduino assembly are retained locally as well.

## Timing and layout

The firmware's 51.2 MHz pixel clock has a 19.53125 ns period and a 9.765625 ns
half-period. Smartwin requires 5 ns setup and hold. With data launched on the
opposite edge, the AXC8's 0.5–4.0 ns propagation range gives a conservative
3.5 ns clock/data mismatch allowance across devices. The adapter's routed RGB
length mismatch contributes up to 0.329 ns using a 7 ps/mm propagation estimate.
ST's DS12930 Table 105 additionally allows 0.5 ns half-cycle error and 0.5 ns
maximum data-valid delay. Under that table's characterization conditions, the
remaining calculated setup/hold margins are only **0.271 ns / 0.437 ns**, before
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

In1.Cu is a continuous ground reference; F.Cu, In2.Cu and B.Cu carry routing.
Signal routes never cut In1. Minimum track/clearance is 0.15 mm; minimum drilled
via is 0.30 mm with 0.65 mm copper diameter. Vias are tented on both outer sides,
including beneath the Hirose bodies. No open via is used in a solder pad.
Global fiducials are provided on both populated sides of the display adapter
and on the GIGA adapter's populated top side.
Fine-pitch mask webs are at least 0.13 mm at the FFC and I2C buffer pads. The FFC
paste windows follow Hirose's 0.25 × 1.00 mm recommendation.

J8 uses 1.55 mm pads with 1.05 mm holes, leaving a 0.25 mm annular ring and space
for routing between contacts. Other stacking headers retain 1.8 mm pads.

## Fabrication and assembly order

Use the separate revision-A ZIP for each board. Select four-layer FR-4, 1 oz
copper on every layer, ENIG, green solder mask, white silkscreen, IPC Class 2 and
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

1. With both supplies off and no panel fitted, check for shorts, all connector
   contacts and all 80 cable conductors. Confirm that GIGA_3V3 and PANEL_3V3 are
   separate, and all specified grounds are connected. Verify cable orientation.
2. Power each side alone with a current-limited source. Confirm no powered-off
   rail is driven through signals, backlight stays off and panel resets remain
   LOW until commanded. Check the protected supply polarity and rise time.
3. Attach the unpowered panel, power normally and run `display bars`. Check colour
   order, all active pixels and both framebuffer slots. Exercise brightness from
   zero to full and `display touch`, including touch reset and interrupt.
4. Measure panel supply at full brightness, input/backlight current, connector
   and protection-component temperatures, PCLK frequency/duty/phase and data
   setup/hold at the panel. Qualify the 33 Ω values and the full 100 mm cable.
5. Test either supply starting first, either side switching off, reconnecting USB
   and explicit display reinitialization. Repeat under graphics/ICC/logging load.

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

Source documents and attribution are listed in `references/SOURCES.md`.
