# Electrical and manufacturing review

Reviewed source: **display adapter D, GIGA adapter C, source bundle D**.
The exact checked source and exported archives are identified in
[`release-status.json`](release-status.json) and the release checksums.

**Local fabrication checks pass. These are prototype fabrication packages,
not an unconditional approval of electrical operation.** RGB timing and dynamic
display-supply behavior remain open qualification items. AISLER's processing of
the exact uploaded archives and physical testing have not been performed.

## Fabrication and connectivity

| Check | Display D | GIGA C |
|---|---:|---:|
| Copper layers | 2 | 4 |
| Finished thickness | 1.6 mm ±10% | 1.6 mm ±10% |
| Copper / finish | 35 µm / ENIG | 35 µm / ENIG |
| ERC findings | 0 | 0 |
| DRC findings after fresh zone refill | 0 | 0 |
| Schematic/PCB parity findings | 0 | 0 |
| Unrouted connections | 0 | 0 |
| Ignored check categories / exclusions | 0 | 0 |
| Minimum component-hole annular ring | 0.300 mm | 0.325 mm |
| Minimum hole separation | 0.508 mm | 0.492 mm |
| Estimated processed fine-pitch mask web | 0.160 mm | 0.160 mm |
| Estimated connector-to-via mask web | 0.152 mm | 0.114 mm |
| Exposed through-vias | 148 | 216 |
| Manufacturing bridge suggestions | 4 | 6 |

Both projects use the pinned AISLER HD templates, ENIG, green mask and white
silkscreen. Copper-to-edge spacing, SMD dimensions, silk strokes, height,
clearance and coverage pass the local audits. There are no fiducials, blind
vias, via-in-pad, tenting requirements or manufacturing-rule waivers. The
display uses its ground pours without redundant backside ground tracks.

The independent connection table covers **50 display contacts, 80 cable
conductors and 35 signals**. It checks every RGB bit, control and touch line,
both connector orientations, power/ground contacts and current CubeMX pin
assignments. No motor/EtherCAT pin conflicts were found. Cable numbering is
GIGA J1.n → display J2.(81−n); use the specified Type-A cable, not a cable chosen
only by conductor count.

Gerbers were parsed and rendered independently of KiCad, including the GIGA
inner layers. Export audits compare pad coordinates/net attributes, drill
positions/diameters/plating, layer counts, outlines and FH28 paste apertures
with the PCB. ODB++ audits check the layer/profile/net/pin exports and bridge
document layer. BOM and placement references agree with the boards.

Evidence: [`electrical-audit.json`](verification/electrical-audit.json),
the per-board `*-drc.json`, `*-erc.json`, `*-aisler-audit.json`,
`*-gerber-audit.json`, `*-odb-audit.json` and `*-bom-placement.json` in
[`verification/`](verification/).

## Electrical design review

- GIGA and panel 3.3 V rails are separate. AXC translator supplies/directions,
  partial-power isolation, startup pull-downs and touch-interrupt pull-ups are
  consistent with the intended direction of every signal. TCA9517A's A side
  faces the panel, B side faces GIGA; the existing GIGA I²C pull-ups are retained.
- Protected 24 V feeds the 12 V GIGA converter and the fused ribbon branch.
  The eFuse, TVS, output clamp and VIN blocking diode have the intended polarity.
  The divider tolerance budget places overvoltage cutoff below the converters'
  36 V input limit. This is not a tested surge-immunity or fault certification.
- The F1→U7 fused-input feed is at least 0.60 mm wide. Actual copper paths to
  the converters, input capacitors, panel-reference return and both GIGA VIN
  contacts are checked, independently of matching pad net names.
- U7's thermal pad and four thermal vias connect the top/bottom **RTN** islands.
  RTN remains isolated from system GND; U7 pin 9 is the GND connection.
- Smartwin backlight contacts are **5–8**, TFT supply is **10**, and touch supply
  is **49**. The power-drop calculation uses these loads at their actual pads.
  The 24 V ribbon feed uses two parallel contacts; the 3.3 V interface reference
  uses three. High backlight current remains on the display board.

Evidence: [`power-review.json`](verification/power-review.json),
[`power-routing.json`](verification/power-routing.json),
[`power-budget.json`](verification/power-budget.json) and both exported
schematics. Resistance estimates use 35 µm copper and a 1.3 hot-resistance
factor; return/contact/efficiency allowances are identified, not measured.

## Open electrical qualification

### RGB timing at 51.2 MHz

The conditional budget leaves only **0.240 ns setup and 0.358 ns hold margin**
after panel requirements, buffer extremes, modeled adapter route skew and
selected STM32 timing terms. Arduino route skew, cable/package effects, actual
load/slew, ringing and clock pulse-width distortion are not fully bounded.
This is **not complete worst-case timing closure**.

ST's quoted output timing assumes enabled IO compensation. No explicit
compensation-enable call was found in the current project firmware. Enable and
settle compensation before using that timing characterization; this review
does not change firmware. Measure clock phase, high/low width and data/control
setup/hold at the panel connector under the intended load and operating range.

A candidate fallback is **40.96 MHz, 1120 × 610 total pixels (~59.95 Hz)**,
within the panel's stated DE-mode timing limits. It adds about 2.44 ns per half
cycle. This is an unimplemented candidate, not a verified remedy. If timing
cannot be qualified, revise the clock configuration or interface before treating
the design as production-ready.

Evidence: [`timing-budget.json`](verification/timing-budget.json),
Smartwin AC timing and the translator/STM32 datasheets in `../references/`.

### Display supply and contact loading

At the **assumed 1.5 A qualification load**, the calculated steady-state minimum
is **3.011 V**, only 11 mV above the panel's specified 3.0 V minimum. This uses
explicit converter tolerance, temperature, ripple, copper and contact allowances;
it does not guarantee the complete operating range.

TRACO specifies up to **250 mV peak deviation for a 50% load step** for this
model group. That excursion is larger than the remaining DC margin. Measure
the panel rail during startup, brightness changes and independent supply
sequencing, requiring 3.0–3.6 V and the panel's 1–20 ms rise-time requirement.
Nominal external output capacitance is 218.8 µF; panel internal capacitance is
unspecified and must also fit the converter's 900 µF limit. Actual MLCC
capacitance and added capacitors' effect on transients need qualification.

Smartwin specifies **1.05 A typical**, not maximum, backlight current. Four
DF23 contacts rated 0.3 A each imply a 1.2 A equal-sharing ceiling; unequal
current sharing and temperature still matter. Verify full-brightness current,
contact/trace temperatures, module derating and eFuse/branch-fuse behavior.
If rail or contact limits fail, change the power design before further release.

Evidence: [`power-budget.json`](verification/power-budget.json), Smartwin DC
specifications and the TRACO/Hirose datasheets in `../references/`.

## Mechanical and manufacturing acceptance

The nominal CAD review checks all 110 GIGA mating contact centres, the 13 mm
carrier spacing, different inner/outer header tails, service cutouts and the
display connector orientation. The display converter leads must be trimmed to
≤0.8 mm below the PCB. They then clear the minimum modeled mating plane by
0.5 mm. Insulating mounts and cable strain relief are required; connector/flex
loads cannot support the boards. The STEP envelopes do not establish real
contact engagement or tolerance fit. See
[`mechanical-audit.json`](verification/mechanical-audit.json).

Upload each **ODB++ upload ZIP** as a separate AISLER bare-board job. Before
ordering, review the exact processed jobs for ENIG, layer count, 1.6 mm
thickness, copper weight, PTH/NPTH holes, mask openings/webs, closed profiles,
bridge positions and netlist processing notes. Resolve any discrepancy.
No job has been uploaded or ordered by this review.

Manufacturer references: [two-layer ENIG rules](https://community.aisler.net/t/2-layer-1-6mm-35-m-enig-design-rules/3732),
[four-layer ENIG rules](https://community.aisler.net/t/4-layer-35-m-enig-design-rules/3733),
[mask processing](https://community.aisler.net/t/soldermask-opening-adjustment/3464),
[via treatment](https://community.aisler.net/t/via-plugging-filling-via-drill-vs-via-size/5522/2).
Support package revision: `00fe215f96ce7a938946e1a176de4ca8cac291ef`.

The release tools refill and save zones before DRC and export, then reject
packaging if checked design inputs changed. Archive hashes identify the exact
reviewed files; re-export and repeat verification after any design edit.
