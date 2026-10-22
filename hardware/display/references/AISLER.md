# AISLER manufacturing inputs

Checked 2026-10-22. Both boards use ENIG, 1.6 mm nominal thickness, 35 µm copper,
green mask and white silk. The order scope is bare boards only.

Support package revision: `00fe215f96ce7a938946e1a176de4ca8cac291ef`.
Exact upstream template checksums are in
[`../manufacturing/aisler-support-lock.json`](../manufacturing/aisler-support-lock.json).
The Git submodule is unchanged. Project constraints supplement the upstream
templates; template rule severities that would hide useful checks are not copied.

| Reference | Applied requirement |
|---|---|
| [4-layer ENIG rules](https://community.aisler.net/t/4-layer-35-m-enig-design-rules/3733) | Both boards: four-layer HD profile; component-hole annular rings ≥0.30 mm |
| [Published 4-layer stackup](https://community.aisler.net/t/4-layers-1-6mm-35-m-stackup/5457) | 0.14 / 1.20 / 0.14 mm dielectrics; 1.6 mm ±10% finished thickness |
| [Mask processing](https://community.aisler.net/t/soldermask-opening-adjustment/3464) | Zero source expansion; model 20 µm enlargement per side and verify ≥0.10 mm fine-pitch webs |
| [Via treatment](https://community.aisler.net/t/via-plugging-filling-via-drill-vs-via-size/5522/2) | Exposed through-vias; no reliance on tenting, plugging or filling |
| [KiCad custom bridges](https://community.aisler.net/t/custom-bridges-kicad-drc/3845) | `aisler-bridges` user layer in ODB++; keep Edge.Cuts closed |
| [Bridges and frames](https://community.aisler.net/t/custom-bridges-and-frames/49) | Review actual processed bridge placement before ordering |
| [Smart Tests](https://community.aisler.net/t/smart-tests/52) | Inspect the uploaded job's electrical-test/netlist notes |

The GIGA custom profile retains conservative 0.15 mm net clearance. The display
uses the HD profile's 0.125 mm clearance for the new dense connector escapes.
Both retain ≥0.15 mm trace widths. Hole-to-hole spacing and copper-to-edge
clearance are ≥0.30 mm; hole-to-copper clearance follows the template's 0.25 mm.
These are distinct KiCad constraints.

Silk strokes are ≥0.15 mm, text height ≥0.8 mm and pad clearance ≥0.125 mm.
Independent Gerber inspection records a conservative ink-coverage bound below
25%. SMD pad dimensions are ≥0.20 mm. All fabrication DRC findings must be fixed;
there are no manufacturing exclusions or accepted manufacturing warnings.

The DF23 drawing **EDC3-164006-03, Fig. 1 note 3** identifies unmasked-contact
bands beneath the connector. Conservative B.Cu rule areas forbid vias there;
the geometric audit also includes the enlarged mask aperture. The retained
Molex 505110 connector's body envelope is audited to exclude exposed via
apertures; no solder-mask coating is relied on to insulate its contact mechanism.
Solder pads and mating space remain clear. The local models are drawing-based envelopes; physical fit is untested.
The retained TRACO TSR1 and TSR2 specifications identify non-conductive plastic housing;
each converter's three conductive leads are confined to their plated pads and
must be trimmed to the documented 0.8 mm protrusion. The GIGA carrier's underside
clearance and insulated stacking sockets separate its open vias from the MCU board.

Local verification and AISLER processing are separate. No AISLER job has been
uploaded or accepted by these scripts. Inspect the exact release ZIP's layer
count, ENIG option, thickness, holes, mask, bridges and netlist notes before
recording manufacturing acceptance. First-board fit, soldering and operation
remain pending even after CAM acceptance.
