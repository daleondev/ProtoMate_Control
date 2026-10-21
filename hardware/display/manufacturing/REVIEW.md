# Electrical and manufacturing review

Reviewed design: **display adapter E, GIGA adapter D, source bundle E**.
Exact source hashes and completed local checks are recorded in
[`release-status.json`](release-status.json); archive hashes are in
[`../releases/SHA256SUMS`](../releases/SHA256SUMS).

These are prototype fabrication packages. Local verification, AISLER's
processing of the uploaded files, and physical operation are separate gates.
No order or upload has been performed by this workflow.

## Implemented design

Both boards use the AISLER **four-layer HD / ENIG** profile: 1.6 mm finished
thickness, 35 µm copper, green mask, white silk, and an uninterrupted In1 ground
reference. The display board is **55 × 50 mm**; its two connector positions are
unchanged, and its lower mounting holes are at y = 47 mm. The GIGA outline,
headers, service access and existing power routing are retained.

The display's power domains are separated:

- **TSR 2-2433** supplies only the four backlight contacts, with 188 µF nominal
  local bulk capacitance. The main copper feed is widened; connector escapes
  remain short. Its load allocation is 1.2 A, the equal-sharing ceiling of four
  0.3 A contacts, not a measured panel maximum.
- **TSR 1-2450 → TPS7A9101DSKR** supplies TFT, touch and interface references.
  The 0.1% feedback divider sets 3.296 V. A 47 nF soft-start capacitor gives a
  component-limit ramp estimate of 3.3–11.3 ms, inside the panel's 1–20 ms range.
  The 0.45 A logic allocation includes TFT, touch and interface loading.
- **Two TPS3808G33 supervisors** require both rails to settle for 300 ms.
  Their wired-AND READY signal gates PWM, standby and both resets through a
  SN74LVC08A. Defined pull-downs retain the inactive state with missing power.
- READY returns over **GIGA J1.10 ↔ display J2.71**, through an AXC1 isolator
  to **PI1 / D70**. Firmware repeats the panel sequence after power recovery,
  including when USB has kept GIGA running. Rendering remains available with
  the panel absent; brightness waits for a valid framebuffer scan boundary.

The GIGA eFuse, branch fuse, 12 V converter, VIN blocking diode, isolated RTN
thermal islands and four RTN vias retain their established circuit. The new
READY connection does not change the 35 original display signals or interfere
with motor/EtherCAT pins. The cable now contains 39 ground conductors, 35 display
signals, READY, two protected 24 V conductors and three logic-reference returns.
PCLK still has ground on both sides.

The firmware configures **40.96 MHz**, 1120 × 610 total pixels (~59.95 Hz),
using DE timing. The bootstrap enables CSI-referenced IO compensation and waits
for readiness. The route-based timing calculation includes translator extremes,
STM32 characterization terms and panel requirements; it reports the remaining
conditional margins in [`timing-budget.json`](verification/timing-budget.json).
Clock phase, loading, ringing and cable/Arduino-board skew still require physical
qualification. The calculated margins are not guaranteed interface closure.

## Local verification

The release pipeline requires all of the following, without waived findings:

- Fresh zone fill, ERC, DRC, schematic/PCB parity and zero unrouted connections.
- Independent comparison of all 50 panel contacts, 80 ribbon conductors,
  original 35 display signals, READY, device pinouts and firmware assignments.
- Exact component MPN, placement and local 3D-model consistency.
- AISLER geometry checks for holes, annular rings, edge clearance, SMD sizes,
  silk, exposed vias and fine-pitch processed mask webs of at least 0.10 mm.
- Gerber/ODB++ layer, outline, pad, net, mask, drill/plating and bridge audits;
  BOM/placement agreement; independent copper rendering and assembly review.
- Routed power resistance and thermal-island checks. Resistance estimates use
  35 µm copper and a 1.3 hot-copper factor. Contact/return allowances and allocated
  currents are explicit; they are not measurements.

There are no fiducials, blind/buried vias, tenting requirements, manufacturing
rule exclusions or accepted ERC/DRC warnings. The display retains its outer
thermal/ground pours; the added inner plane supplies a continuous reference.

Evidence is retained in [`verification/`](verification/). Current component-limit
budgets are in [`power-budget.json`](verification/power-budget.json); routed
losses are in [`power-routing.json`](verification/power-routing.json). The logic
calculation reserves **100 mV dynamic drop** in addition to static tolerances,
copper, contacts and return losses. The budget includes independent 25 ppm/K
feedback-resistor drift over 100 K and the LDO’s 100 nA feedback leakage.
LDO temperature is estimated using the
datasheet's JEDEC thermal resistance at 55°C enclosure ambient and 0.45 A;
actual board/enclosure temperature must be measured.

Combined M4/M7 Debug and Release firmware and the Linux application build successfully.
Seven focused boot/HAL/GUI/lifecycle regression suites pass.
The HAL tests cover missing-power startup, valid-frame gating, brownout during
startup, power loss/recovery with a static framebuffer, and shutdown. These are
software tests; no physical panel or assembled adapter was tested.

## Assembly and first-board acceptance

The **TPS7A91 WSON exposed pad requires reflow or hot-air assembly**, including
solder beneath the package. Edge-lead hand soldering alone is insufficient.
The small 0.5 mm Hirose connectors also need suitable magnification and process
control. Use the procurement BOM and assembly drawings, not silkscreen alone,
to establish pin 1 and bottom-side connector orientation.

Trim both display converters' tails to **≤0.8 mm below the PCB**. The nominal
mating review leaves 0.5 mm clearance at the modeled minimum connector spacing;
lead trim, actual mating fit and tolerances must still be checked. Use insulated
mounts and cable strain relief. Neither the connector nor the display flex is a
structural mount. The GIGA mating model checks every header centre and the
13 mm carrier spacing; simplified envelopes do not certify contact engagement.

Before fitting the panel, check continuity, supply isolation, polarity, unloaded
rails and the READY/gate sequence. Then qualify:

1. Logic rise time and voltage at TFT/touch contacts, startup ordering, USB-only
   operation, 24 V removal/return and brownout recovery.
2. Full-brightness current and contact sharing, rail ripple, PWM/load transients,
   fuse/eFuse behavior, LDO/converter temperature and enclosure airflow.
3. Pixel-clock phase/high-low widths, setup/hold, colour order, stable display,
   touch operation and independent power sequencing.

The backlight converter specifies up to **250 mV** excursion for a 50% load step.
This can cross the 3.07 V supervisor threshold. Verify that actual PWM operation
with the installed capacitance does not retrigger supervision; do not bypass
supervision to conceal an inadequate supply. Smartwin gives typical backlight
current, not a guaranteed maximum. All four contacts must stay within their
individual 0.3 A ratings. Keep total converter output capacitance, including the
panel, within 900 µF. These are first-board acceptance requirements.

Use the revision-E Type-A cable drawing: 80 ways, 0.50 mm pitch, 0.30 mm mating
thickness, same-side contacts, ≤100 mm, ≥60 V / 0.5 A per conductor and copper
cross-section ≥0.0105 mm². No exact off-the-shelf cable MPN has been qualified;
procure to the drawing if necessary. Check continuity/orientation before power.

## AISLER acceptance

Upload the two **ODB++ upload ZIPs** as separate bare-board jobs. Review the
processed layer count, ENIG option, 1.6 mm thickness, holes/plating, mask webs,
closed outline, bridges and netlist-processing notes for those exact archives.
Resolve discrepancies before ordering. AISLER CAM acceptance remains pending.

Manufacturer rules and the pinned support-package revision are retained in
[`../references/AISLER.md`](../references/AISLER.md). Rerun the complete pipeline
and review before packaging after any layout or component change.
