# Electrical and manufacturing review

Reviewed design: **display adapter F, GIGA adapter E, source bundle F**.
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
headers and service access are retained. J10 is a through-hole DC-005 barrel
jack using the supplied AISLER footprint, facing the service opening. It accepts
a 5.5 × 2.1 mm centre-positive 24 V plug; both sleeve-related pins are grounded.
The local input-protection placement and power routing clear the jack body and
its three plated slots. The electrical protection circuit is unchanged.

F1 is **SF-1206SA200M-2**, 2 A / 65 VDC slow-blow with a 50 A interrupt rating.
Its dedicated 1206 footprint follows Bourns' 1.45 × 1.80 mm land pattern with a
1.50 mm gap; F2 retains its separate footprint and rating. The input-fuse budget
includes 0.140 Ω typical cold resistance (±25%) and approximate 55°C derating,
with an additional 25% continuous-load margin. Startup and fault-clearing tests
remain first-board checks; cold resistance does not bound hot-state dissipation.

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
- READY returns over **GIGA J1.9 ↔ display J2.52**, through an AXC1 isolator
  to **PI1 / D70**. Firmware repeats the panel sequence after power recovery,
  including when USB has kept GIGA running. Rendering remains available with
  the panel absent; brightness waits for a valid framebuffer scan boundary.

The GIGA eFuse, branch fuse, 12 V converter, VIN blocking diode, isolated RTN
thermal islands and four RTN vias retain their established circuit. The new
READY connection does not change the 35 original display signals or interfere
with motor/EtherCAT pins. The cable now contains 19 ground conductors, 35 display
signals, READY, two protected 24 V conductors and three logic-reference returns.
PCLK still has ground on both sides.

The firmware configures **40.96 MHz**, 1120 × 610 total pixels (~59.95 Hz),
using DE timing. The bootstrap enables CSI-referenced IO compensation and waits
for readiness. The route-based timing calculation includes translator extremes,
STM32 characterization terms and panel requirements; it reports the remaining
conditional margins in [`timing-budget.json`](verification/timing-budget.json).
Clock phase, loading, ringing and cable/Arduino-board skew still require physical
qualification. The calculated margins are not guaranteed interface closure.
The panel samples on the falling pixel-clock edge; the configured LTDC clock
polarity places data transitions on the opposite edge. `LTDC_DEPOLARITY_AL`
is intentional: ST's LTDC inverts DE, so this produces HIGH during active pixels
(ST AN4861, control-signal polarity configuration). These settings still require
connector-level measurement on the assembled boards.

## Local verification

The release pipeline requires all of the following, without waived findings:

- Fresh zone fill, ERC, DRC, schematic/PCB parity and zero unrouted connections.
- Copper connectivity checked independently of DRC: no duplicate/covered
  tracks or unused signal branches/vias. Short full-width pad entries are
  individually reviewed and trial-removed in a scratch project: some preserve
  minimum connection width, while others provide a centred pad termination
  instead of relying on a trace cap touching the pad edge. Ground/thermal stitching is reviewed
  for its interlayer function rather than treated as redundant signal routing.
- Independent comparison of all 50 panel contacts, 60 ribbon conductors,
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
Ground tracks are retained only where needed for pad-to-plane escapes; filled
copper and stitching vias provide the common return. GIGA component-to-plane escapes and the isolated
eFuse RTN thermal connections remain in place.
The layer-by-layer visual review covers connector fanouts, power routing,
centred via entries, angular corners, silkscreen and the exported board views.
The machine-readable copper audits record each intentionally retained pad-entry
segment and its geometric or connection-width purpose; these are intentional
pad terminations, not waived manufacturing findings.

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
Firmware holds the backlight off for 200 ms after standby release, then requires
a valid framebuffer scan boundary. This allows the panel's multi-frame internal
power sequence in Smartwin page 15 to finish. A regression test checks that valid
frames during the early startup interval cannot turn the backlight on.

The final independent order review is recorded in
[`order-review.json`](verification/order-review.json). It binds the checks to the
two exact ODB++ archives and lists the remaining physical/procurement conditions.
No PCB connectivity or local manufacturing-rule defect remains identified.
This is approval for prototype fabrication, not certification of a functioning
assembled system or a substitute for AISLER's processed preview.

## Assembly and first-board acceptance

The **TPS7A91 WSON exposed pad requires reflow or hot-air assembly**, including
solder beneath the package. Edge-lead hand soldering alone is insufficient.
The small 0.5 mm Hirose/Molex connectors also need suitable magnification and process
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
The calculated static contact voltage is **3.029 V** at the 1.2 A allocation.
The 50 mV downward ripple allowance exceeds the quoted 50 mV peak-to-peak
typical value but is not a manufacturer-guaranteed bound. Therefore the 29 mV
static margin cannot be presented as a worst-case transient guarantee.

Use **Molex 15018-0563** with two **505110-6091** bottom-contact sockets:
60 ways, 0.50 mm pitch, 0.30 ±0.05 mm mating thickness, same-side gold contacts
and retention ears. The nominal cable length is 102 mm; budgets use 106 mm.
The cable has 0.30 × 0.10 mm copper and a 60 V / 0.5 A rating; the sockets limit
the assembled link to 50 V / 0.5 A per contact. Nineteen shared grounds include
both PCLK neighbours. The new pin map is **n ↔ 61−n**. Both board revisions must
be used together; check continuity and orientation before power. Procurement
includes Mouser SKUs for this exact cable and both sockets.

The Molex land pattern uses 0.30 × 1.00 mm signal pads and 2.00 × 1.30 mm
mounting pads. Paste is 100% land area for the manufacturer's 0.10 mm stencil
reference. Exposed vias are excluded from the connector's contact mechanism.
Firmware assignments and power architecture are unchanged by this cable update.

## AISLER acceptance

Upload the two **ODB++ upload ZIPs** as separate bare-board jobs. Review the
processed layer count, ENIG option, 1.6 mm thickness, holes/plating, mask webs,
closed outline, bridges and netlist-processing notes for those exact archives.
Resolve discrepancies before ordering. AISLER CAM acceptance remains pending.

Manufacturer rules and the pinned support-package revision are retained in
[`../references/AISLER.md`](../references/AISLER.md). Rerun the complete pipeline
and review before packaging after any layout or component change.
