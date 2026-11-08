# Evaluated robot CAD geometry

Source: `SCARA_Robot.step`, exported 2026-11-04, and the four Fusion COLLADA
visual groups in `src/simulation/assets/robot/`. These are **nominal CAD
measurements, not calibration of the assembled robot**. `geometry.json` records
source SHA-256 hashes, all 372 STEP body bounds, all 372 visual body bounds,
selected analytic cylinders, assembly references, registration checks, and
verbatim relevant product descriptions. The measurements themselves do not use collision STLs. Native simulation uses separate CAD-derived rotary cam bodies and the supplied
Axis 3 collision STL for switch contacts.

Reproduce with FreeCAD 1.1 / OpenCascade 7.9 installed:

```sh
python3 scripts/extract_robot_geometry.py
python3 scripts/extract_robot_geometry.py --check
```

Use `--freecad-lib PATH` if FreeCAD's Python modules are installed elsewhere.
FreeCAD is an offline inspection dependency only. The application and normal
build/tests do not require it. The normal `simulation.cad_assets` test checks
source hashes and correspondence between the report and C++ constants.

## Frames and kinematics

All coordinates below are millimetres, right-handed, Z up, X along the extended
arms. The exported STEP pose has both rotary coordinates zero. The simulator starts in this pose. Its Z joint is displacement from CAD rest,
with zero travel corresponding to a TCP height of 28.9 mm above the base foot
plane. Analytic cylindrical axes determine the rotary centers; the J3 assembly
placement determines the Z-stage attachment datum.

| Quantity | Evaluated value | Evidence |
|---|---:|---|
| J1 axis XY | (0, 0) | B07 tube, outer cylinder radius 12.5 |
| J2 axis XY | (205, 0) | J2-07 tube, outer cylinder radius 12.5 |
| J1 → J2 | **205** | Distance between those axes |
| J2 → J3 attachment datum | **223.4** | J3 datum X=428.4 minus J2 X=205 |
| J3 datum → tool axis | **135** | Cutter axis X=563.4 minus datum X=428.4 |
| Effective second arm, to TCP | **358.4** | 223.4 + 135 |
| Fully extended nominal reach | **563.4** | 205 + 223.4 + 135 |
| Rest TCP | **(563.4, 0, 28.9)** | Analytic cutter axis and lower end plane |
| Cutter reference | Ø3.2 × 25 | Approximate Dremel/tool envelope; not actual tool length calibration |

The visuals are already baked into four separate link frames. They are not
world-space meshes and must not be centered automatically:

| Visual group | Translation into STEP at rest | Rotation |
|---|---|---|
| `base_link` | (0, 0, 0) | Identity |
| `link_1` | (0, 0, 52) | Identity |
| `link_2` | (205, 0, 369.9) | Identity |
| `link_3` | (563.4, 0, 308.9) | Identity |

The cutter tip is (0, 0, **−280**) in `link_3`. Registration uses planar link
beams, a guide block and a switch plunger; independent second-body checks on
each link agree within 0.00001 mm of exported float vertices. That numerical
agreement does not establish physical accuracy. Rounded frame translations in
`CadGeometry.hpp` remove micrometre-scale floating-point export noise.

Let `T` denote translation and `Rz` rotation about Z. Rendering uses:

```
base     = identity
shoulder = T(0, 0, 52) Rz(q1)
forearm  = shoulder T(205, 0, 317.9) Rz(q2)
carriage = forearm T(358.4, 0, 308.9 - 369.9 + qZ)
tcpZ     = 28.9 + qZ
```

This puts the rendered cutter tip at exactly the simulator's forward-kinematic
TCP, including multi-turn angles. Positive rotary signs are the simulator's
right-handed convention; physical motor wiring/direction remains unverified.
The four groups articulate rigidly; internal pulley rotation, belt circulation
and bearing rolling motion are not separately animated by these link exports.

## Structure and drives

Dimensions use evaluated global geometry and tight OpenCascade bounds, without
triangulation or shape-tolerance inflation; cylindrical sizes use analytic
surfaces. Full body extents and positions are retained in `geometry.json`.

| Part | Evaluated nominal geometry |
|---|---|
| Base rails, two | 40 × 40 × 250; Z=12…52 |
| Base feet | Combined footprint 250 × 230; thickness 12 |
| J1 tube | OD25 / ID22 × 160; Z=54…214 |
| J2 tube | OD25 / ID22 × 100; Z=247…347 |
| Link 1 beam | 200 × 60 × 30; X=5…205, Z=216.5…246.5 |
| Link 2 beam | 200 × 60 × 30; X=255…455, Z=266.9…296.9 |
| Z uprights, two | 40 × 20 × 280; Z=68.9…348.9 |
| Z rear crossmembers | 20 × 72 × 20; center heights 98.9 and 238.9 |
| Z guide rails, two | 8 × 12 × 200; Z=83.9…283.9, Y centers ±26 |
| Z guide blocks, four | 10 × 27 × 45.4; vertical center spacing 60 |
| Z screw envelope | Ø8 × 350; axis (522.4, 0), Z=66.9…416.9 |
| J1 pulley centers | 96.624621 apart |
| J2 pulley centers | 64.940142 apart |
| Z pulley centers | 60 apart |

Selected pulley labels specify J1 **100T/20T**, J2 **100T/20T**, and Z
**20T/20T**, giving nominal motor reductions **5:1, 5:1, 1:1**. The screw's
product description explicitly **assumes M8×1.25 pitch**. The STEP models a
smooth thread envelope, so **1.25 mm per screw revolution is metadata, not a
geometric measurement**. Native `Profile` now uses those nominal values instead
of the previous 6:1 / 8 mm placeholders. The product controller's archived
mechanical test fixtures and physical-motion admission are unchanged.

## Home sensor mounting

Coordinates refer to the center of each plunger solid in its supplier free
position. They are not the free lever tip or an electrical switching point.

| Sensor | Parent visual link | Local plunger center | Rest world center | Plunger outward axis |
|---|---|---|---|---|
| J1 HOME | `base_link` | (−78.7, 0.4, 195.2) | (−78.7, 0.4, 195.2) | +X |
| J2 HOME | `link_1` | (154.1, 0.4, 221.8) | (154.1, 0.4, 273.8) | +X |
| Z HOME | `link_2` | (321.6, 49.7, −106.5) | (526.6, 49.7, 263.4) | −Y |

The PCB mounting holes have radius 1.5 and 15 mm separation. Cylinder axis
points at the modeled PCB surface, in the rest world frame, are:

- J1: (−97.5, −7.5, 192.3), (−97.5, 7.5, 192.3), axes −Z.
- J2: (135.3, −7.5, 270.9), (135.3, 7.5, 270.9), axes −Z.
- Z: (523.7, 68.5, 256.3), (523.7, 68.5, 271.3), axes −X.

The curved fixed roots of the modeled spring levers provide the geometric
flexion axes, also retained in `geometry.json` and `cad::LEVER_ROOTS_MM`:

| Sensor | Local root point | Press rotation axis |
|---|---|---|
| J1 | (−79.2, 2.800388, 195.2) | −Z |
| J2 | (153.6, 2.800388, 221.8) | −Z |
| Z | (321.6, 50.2, −108.900388) | −X |

Native homing now uses collisions between each extracted lever and the next
moving link's extracted rotary cam body (Axis 1/2) or supplied collision STL
(Axis 3). Parent-relative transforms follow actual
converted motor motion. The free and nominally deflected lever must both be
contacted to activate; backing clear releases the contact. The real controller
reads those transitions through its normal NC inputs.

CAD gives neither electrical actuation travel nor hysteresis. The simulator
therefore uses explicit, adjustable **assumptions**: 2° rigid lever flexion and
0.05 mm clearance for release. Rotary cam geometry now uses exact visual CAD
surfaces; the Axis 3 collision STL remains an approximate envelope. Neither the derived contact angles nor
controller homing coordinates are physically calibrated switch positions.
The switch model does not imply general rigid-body dynamics or collision safety.

## Conflicts and remaining unknowns

Evaluated geometry takes precedence over stale prose for lengths and placement:

- Old Link 1 text describes 200 mm center spacing / 180 mm cut; current geometry
  is 205 mm center spacing / 200 mm cut.
- Old Link 2 text describes 200 mm / 3030 profile; current geometry is a 223.4 mm
  attachment offset and a 200 mm 3060 beam.
- Old base text and the J1 shaft body name describe obsolete dimensions; the
  evaluated base and hollow 160 mm tube are recorded above.
- J2's long assembly description still specifies 80T/20T, 4:1, and a 40°→10°
  motion link. Current selected component labels specify 100T/20T, 5:1.
- J1's pulley description says 3MGT, while its label says 2GT. Pulley geometry
  is simplified. Procurement/belt pitch is not approved by this extraction.
- A 70 mm Z stroke is mentioned in prose. Static STEP does not establish usable
  travel, safe limits, collision clearance, switch activation or stopping room.
- The Dremel, cutter, threads and several purchased components are explicitly
  approximate envelopes. Physical dimensions, signs, reference offsets and
  tooling calibration still need measurement before hardware commissioning.
