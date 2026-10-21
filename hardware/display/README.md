# Smartwin–GIGA display adapters

The **four-layer display adapter (revision E)** and **four-layer GIGA adapter
(revision D)** connect the Smartwin **SMMH0701024600S-C20** to the Arduino
**GIGA R1 WiFi**, preserving the firmware's display pin assignments.
Open either project in KiCad 10; both use `libraries/` and require no downloaded
symbol or footprint library. Keep this directory structure when moving them.

Release files:

- [Display E: AISLER ODB++ upload](releases/display-adapter-rev-E-aisler-odb.zip)
- [GIGA D: AISLER ODB++ upload](releases/giga-adapter-rev-D-aisler-odb.zip)
- [Display E: review and soldering package](releases/display-adapter-rev-E-aisler-review.zip)
- [GIGA D: review and soldering package](releases/giga-adapter-rev-D-aisler-review.zip)
- [Source bundle E: projects, libraries, models and tools](releases/smartwin-giga-rev-E-source.zip)
- [Nominal GIGA mating assembly](assembly/giga-mated-rev-D.png)

Upload only the two ZIPs labelled **AISLER ODB++ upload**, one job per board.
The review packages contain fabrication references, procurement BOMs, placement
and soldering drawings. They are not assembly orders. `releases/SHA256SUMS`
identifies the exact archives.

| Project | Board | AISLER profile | Finish |
|---|---|---|---|
| `display-adapter/display-adapter.kicad_pro` | Rev E, 4 layers, 55 × 50 × 1.6 mm | `aisler-4-layer-hd-drc` | ENIG |
| `giga-adapter/giga-adapter.kicad_pro` | Rev D, 4 layers, 111 × 53.34 × 1.6 mm maximum | `aisler-4-layer-hd-drc` | ENIG |

Both are **bare boards**, with 35 µm copper on every layer, green solder mask
and white silkscreen. All components are sourced and installed separately.
**AISLER processing acceptance is pending** until the exact upload previews,
options, bridges and netlist notes have been inspected. Physical qualification
also remains pending; local verification does not establish either acceptance.
The [electrical and fabrication review](manufacturing/REVIEW.md) records the
current release limits. These are prototype fabrication packages; full RGB
timing and dynamic supply qualification are still open.

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
24 V -> shield J10 -> fuse / TVS / TPS26600
                        |                  |
                   TSR 1-24120       0.8 A ribbon fuse
                      12 V                 |
                   blocking D3          80-way FFC
                        |                  |
                     GIGA VIN       +------+------+
                                    |             |
                                TSR 2-2433    TSR 1-2450
                                3.3 V BKL        5 V
                                    |         TPS7A91
                                backlight    3.296 V logic
                                                  |
                                      TFT / touch / buffer reference
Both panel rails -> supervisors -> READY -> reset/standby/PWM gates + GIGA D70
```

Connect the existing **regulated 24 V supply to shield J10: pin 1 = +24 V,
pin 2 = GND**. Allow at least 2 A supply capacity for this assembly; the input
protection limits combined current to approximately 1.49 A. The recommended
supply range is 24 V ±10%. Keep grounds common with the robot. This input powers
both GIGA and the display; the display adapter has no separate power terminal.

- **Shield U8: TRACO TSR 1-24120**, 24 V to 12 V, rated 1 A output. D3 feeds
  VIN through a Schottky diode, so actual VIN is slightly below 12 V. VIN reaches
  both corresponding GIGA header contacts, J5.8 and J9.24.
- **Display U1: TRACO TSR 2-2433**, 3.3 V / 2 A, supplies only the backlight
  contacts 5–8. Four local 47 µF capacitors support its load. Keep total output
  capacitance, including the panel, below the module's 900 µF limit.
- **Display U2: TRACO TSR 1-2450**, 5 V / 1 A, feeds **U3 TPS7A9101DSKR**.
  The precision LDO supplies TFT pin 10, touch pin 49 and the interface-reference
  return. Its 31.2 kΩ / 10 kΩ, 0.1% divider sets **3.296 V**. The regulator's
  1% line/load/temperature accuracy gives much more room inside the panel's
  3.0–3.6 V range. The 47 nF soft-start capacitor sets an approximately 6 ms ramp;
  the component-limit estimate is 3.3–11.3 ms. Keep SS_CTRL grounded.
- The logic load allocation is **0.45 A**; the backlight contact ceiling is
  **1.2 A** total, subject to current sharing among its four 0.3 A contacts.
  Allow up to **0.75 A at GIGA VIN** in the combined input budget. These are design
  allocations, not measured panel maxima or simultaneous converter nameplate ratings.
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

The fused-input feed from F1 to U7 uses 0.60 mm copper throughout.
**U7 RTN and its exposed pad must not be connected to GND.** They use separate
top and bottom thermal copper islands joined by four vias. Pin 9 alone is the system-ground
connection. D2 clamps negative excursions on the protected output. F2 is a
**0.8 A/63 V time-lag fuse** for the ribbon branch. Two parallel contacts carry
24 V; at the allocated backlight/logic loads, 18 V input and an 80% efficiency allowance,
each carries approximately 0.216 A, below the connector's 0.5 A/contact rating.
The specified cable copper and contacts have under 0.10 V allocated drop in
that conservative load case. Power is disconnected before inserting/removing
the ribbon; these connectors are not hot-plug power connectors.

All four backlight contacts (J1 **5–8**) are connected. TFT uses **10**, touch
uses **49**. Smartwin specifies backlight current as 1.05 A typical, not a maximum.
Its connector table specifies 3.3 V, so this design keeps that voltage despite
broader input ratings in the internal backlight-driver description.

`manufacturing/verification/power-budget.json` records the component tolerances,
soft-start range, thermal estimate and load allocations. `power-routing.json`
calculates copper losses from the actual layout, including shared path currents.
The logic budget reserves **100 mV for dynamic drop** in addition to regulator,
route, return and contact losses; this is an allowance to qualify, not a guaranteed
transient specification. The LDO's exposed pad is soldered to ground copper with
external thermal stitching. Qualify operation at up to **55°C enclosure ambient**
for the 0.45 A allocation; the thermal estimate is not a measured enclosure result.

The backlight converter is independent. Its specified load-step excursion can
reach 250 mV. The added bulk capacitance and actual PWM load therefore need a
first-board test for rail dips and supervisor retriggering. The supervisors inhibit
the panel on undervoltage; they do not make an undersized supply acceptable.

## Interfaces and startup

- U1–U4 are SN74AXC8T245PWR, with VCCA = GIGA and VCCB = panel. **DIR1 is HIGH,
  DIR2 is LOW**, which selects A→B for all eight channels. Their `/OE` inputs are
  LOW. Each MCU input has a 47 kΩ pull-down. The display-side gate inputs have 47 kΩ pull-downs and their outputs have
  10 kΩ pull-downs, keeping PWM/standby LOW and resets asserted when inactive. The AXC devices provide partial-power-down isolation.
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
- Display **U4/U6 TPS3808G33** supervise the logic and backlight rails. Their
  open-drain RESET outputs are wired together as **PANEL_READY**, pulled up to
  the logic rail. Both supplies must remain above the nominal 3.07 V thresholds
  for **300 ms** before release. VDD and MR use the logic rail, so the supervisors
  stay valid whenever the control gates are powered.
- Display **U5 SN74LVC08A** AND-gates PWM, standby and both resets with READY.
  A brownout clamps them without depending on the GUI, USB or polling.
- Shield **U9 SN74AXC1T45** translates READY back to **PI1 / D70 / J9.15**,
  with a 10 kΩ pull-down on the MCU side. This also handles a disconnected or
  unpowered adapter. READY uses shield FFC contact **10**, display FFC **71**.
- Firmware continues rendering when the panel is absent. After READY returns,
  it holds reset LOW for 2 ms, releases reset, waits 10 ms, releases standby,
  waits 60 ms, then waits for a valid framebuffer scan before restoring the
  requested brightness. Touch acquisition retries separately after I²C failure.
  Initial brightness is 10%; USB-only operation keeps the physical backlight off.

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
pin 81−n**. Thirty-nine conductors are ground, 35 carry display signals, one returns READY,
two carry protected 24 V, and
three return the panel supply reference. PCLK has ground on both sides.

| Rail | GIGA J1 contacts | Display J2 contacts |
|---|---|---|
| Protected, branch-fused 24 V | 1, 3 | 80, 78 |
| PANEL_3V3 reference return | 5, 7, 9 | 76, 74, 72 |
| PANEL_READY return → GIGA D70 | 10 | 71 |

Use the **display-E / GIGA-D pair** and its revision-E cable drawing.
Flash the updated bootstrap and UI firmware together using the repository’s
combined GIGA flash workflow; both the pixel clock and READY sequencing changed. The cable
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

Install all components yourself or through a separately arranged process. The bottom-side
DF23 and both FFC connectors require particular care. U1/U2 on the display adapter
and J2–J10 plus U8 on the GIGA adapter are marked **HAND** in the full BOM; the
SMD-only BOM and placement file are assembly aids, not an AISLER assembly order. Solder stacking headers with a mechanical jig or
an unpowered GIGA used only as an alignment fixture; avoid heating the board's
existing sockets unnecessarily. Fit shield J10 with its wire opening
facing left into the central opening. Probe pads and mounting holes are PCB
features, not purchased parts.

After soldering display U1/U2, **clip their leads to at most 0.8 mm below the PCB**, without
cutting into the solder joints. The local assembly model includes this trim.
This keeps the leads above the display flex's nominal 1.5 mm mating plane.

**Display U3 is a WSON with an exposed pad.** Use controlled hot-air/reflow
and solder paste; edge-only iron soldering cannot solder its thermal pad.
Inspect the joints before applying power. No filled or tented vias are needed.

The local STEP models for Hirose connectors, terminal, converter/eFuse envelopes and stacking sockets are
dimensioned mechanical envelopes derived from manufacturer drawings. They
simplify latch/contact details and are labelled accordingly; they are not
manufacturer production-tooling models. Standard IC/passive models and the
official Arduino assembly are retained locally as well.

## Timing and layout

Firmware uses **40.96 MHz**, with **1120 × 610** total timing and
1024 × 600 active pixels: approximately **59.95 Hz**. Horizontal timing is
1 sync + 47 back porch + 1024 active + 48 front porch; vertical timing is
1 + 4 + 600 + 5. Both CubeMX and the shared bootstrap configure PLL3 /5 ×128 /10.
The bootstrap enables CSI and waits for the IO-compensation cell before either
application starts; LTDC pins use very-high GPIO speed.

The 12.207 ns half-period is budgeted against Smartwin's 5 ns setup/hold,
the AXC8 inter-device propagation spread, STM32 output timing and measured
adapter routing. See `timing-budget.json` for the resulting conditional margins.
Arduino traces, cable skew, actual capacitive loading, settling and clock duty
cycle still require connector-level measurement. The cable limit is **100 mm**;
use a shorter specified cable when the enclosure permits. The 33 Ω series
resistors are fitted starting values, to be qualified with the actual panel.

**Both boards are four-layer**, with an uninterrupted In1 ground reference.
Display signal routing uses F.Cu, In2.Cu and B.Cu, supplemented by stitched outer
ground copper. The dedicated backlight supply uses wide traces. The source
retains the GIGA power-route geometry; its fused input remains at least 0.60 mm.

Vias are exposed through-vias, with no dependence on tenting, plugging or filling.
DF23 exposed-contact bands are via keepouts. Ordinary molded IC bodies may have
vias underneath; no via is in a solder pad. There are no fiducials.
Source mask expansion is zero. The independent mask audit applies AISLER's
20 µm per-side processing enlargement and requires at least 0.10 mm webs around
fine-pitch contacts. FH28 paste windows follow Hirose's 0.25 × 1.00 mm drawing.

J8 uses 1.70 mm pads with 1.05 mm holes (0.325 mm annular rings). U8’s
narrow pad dimension is 1.70 mm with its original 1.00 mm drill. Other stacking
headers retain 1.8 mm pads. All component-hole rings are at least 0.30 mm.

## AISLER upload and manufacturing review

1. Upload `display-adapter-rev-E-aisler-odb.zip` and
   `giga-adapter-rev-D-aisler-odb.zip` as separate AISLER projects.
2. Confirm **4 layers on both, ENIG on both, 1.6 mm ±10%, 35 µm copper**,
   green mask and white silk. Select bare boards only; no assembly or stencil.
3. Inspect the processed copper, holes and mask. Confirm all 50 display contacts,
   80 FFC contacts, separate plated/non-plated drills, exposed vias and fine webs.
   Check netlist/Smart Tests processing notes against the supplied netlists.
4. Inspect four display and six GIGA bridge markers from `aisler-bridges`.
   The actual profile must remain closed. Keep bridges clear of the connector
   mating edges and service cutouts. Resolve any missing or relocated marker
   with AISLER before ordering. Trim remnants before soldering components.
5. Compare the processed preview with the independent Gerbers and assembly
   drawings, then record the AISLER project/revision and acceptance. No order
   has been placed by this work.

The four-layer stack uses the published 0.14 / 1.20 / 0.14 mm dielectric
construction with four 35 µm copper layers. The nominal layer sum is 1.62 mm,
within the specified 1.6 mm ±10% finished thickness. AISLER controls the
production laminate.
Concave outline corners use 0.9 mm radii for a 1.8 mm milling tool.

The display PCB is 1.6 mm thick. DF23 mating separation is measured between
PCB surfaces and remains nominally 1.5 mm (minimum 1.3 mm). Converter tails
trimmed to 0.8 mm leave 0.5 mm at that minimum separation. Check the top-side
enclosure envelope and supports against the finished thickness tolerance.
The GIGA adapter retains its 13 mm underside clearance and header geometry.

The package retains Gerbers for every copper/mask/silk/paste layer, separate
PTH/NPTH drills, IPC-D-356 and schematic netlists, schematics, STEP models,
fabrication drawings, full BOM, SMD placement aids and cable specification.
Gerber/drill/place coordinates use the lower-edge auxiliary origin; ODB++ uses
absolute KiCad X and negated Y. Each format is internally audited.
No bottom fabrication layer is mirrored manually.

The release pipeline requires zero ERC/DRC/parity findings, zero unrouted
connections and no manufacturing waivers. Processed mask estimates, copper
connectivity and source hashes are recorded in `manufacturing/verification/`.
Only the matching checksummed release ZIPs are manufacturing inputs.

Rules are based on support revision
`00fe215f96ce7a938946e1a176de4ca8cac291ef`. The upstream submodule remains unchanged.
The templates are supplemented by component-hole rings, silk, processed-mask,
exposed-via, bridge and mechanical audits; there are no manufacturing waivers.
See [manufacturer references and locked inputs](references/AISLER.md).

## First-board acceptance

1. With 24 V and USB disconnected and no panel fitted, check for shorts, all connector
   contacts and all 80 cable conductors. Confirm that GIGA_3V3 and PANEL_3V3 are
   separate, and all specified grounds are connected. Verify cable orientation.
2. Apply current-limited 24 V with GIGA/panel initially unplugged. Check U8's
   12 V, VIN after D3, 5 V preregulator and both display 3.3 V rails before attaching loads.
   Verify UV/OV cutoffs and current limiting using a bench supply. Then test
   USB-only operation: no 24 V or panel-3.3 V backfeed, backlight off, resets LOW.
3. Attach the unpowered panel, power normally and run `display bars`. Check colour
   order, all active pixels and both framebuffer slots. Exercise brightness from
   zero to full and `display touch`, including touch reset and interrupt.
4. Measure panel supply at full brightness, input/backlight current, connector
   and protection-component temperatures, PCLK frequency/duty/phase and data
   setup/hold at the panel. Qualify the 33 Ω values and the full 100 mm cable.
5. Test 24 V first, USB first, removal of either supply and automatic display
   reinitialization after power recovery. Verify READY timing and hardware gating. Repeat under graphics/ICC/logging load.

## Reproducibility

`tools/build.py` regenerates libraries/schematics and **placement-only boards in
`build/`**. It does not overwrite reviewed project routing. `prepare_routes.py`
adds the initial power and ground escapes. Routing tools are generation aids;
the checked-in project boards are the authoritative layouts.

`tools/audit.py` independently compares the exported schematic netlist, PCB pad
nets, manufacturer pin tables, all cable conductors and the CubeMX pin assignment.
`tools/export_release.py` requires clean ERC/DRC after a fresh KiCad CLI zone
refill and checks BOM/placement agreement. `tools/review_power.py` verifies
actual power paths, input-capacitor connections and the RTN thermal islands.
`tools/inspect_gerbers.py` parses the exported files with Gerbonara, independent of
KiCad, and checks exported pad/drill coordinates and layer geometry. Release
archives are made only after these checks and visual/mechanical review.

`tools/design.py` defines board revisions and layer/via settings;
`tools/aisler.py` applies the pinned manufacturing profiles without replacing
routing. The source bundle revision is E. The source bundle includes the same
support templates and a hashed CubeMX pin snapshot for offline reproduction.
In the full repository, the electrical audit checks the current CubeMX file;
outside it, the report explicitly identifies the recorded release assignments.

Use Python with pcbnew, NumPy, Pillow, Shapely, Matplotlib and Gerbonara, plus
KiCad 10, FreeCAD and Inkscape. `tools/release.py` runs the local checks and
exports; review the generated drawings and 3D assembly before packaging with
`tools/package_release.py --reviewed`. Packaging rejects PCB, schematic, project
or rule changes made after verification. The manufacturing status separates local
validation from pending AISLER processing and first-board qualification.

Source documents and attribution are listed in `references/SOURCES.md`.
