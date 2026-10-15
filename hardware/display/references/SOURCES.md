# Design sources and attribution — display C / GIGA B

Reviewed 2026-10-15. Local copies retain the manufacturers' copyright. They are
engineering references, not project-authored specifications.

| Source | Use in this design |
|---|---|
| Smartwin `SMMH0701024600SC20.pdf`, supplied in `docs/display` and copied here | All 50 contacts, power, timing, reset, touch and rear-view mating orientation |
| Hirose `DF23C-50DP-0.5V-92-CL0688-0804-6-92-2DDRAWING-0000931633.pdf`, supplied in `docs/display` and copied here | Plug land pattern, numbering, body envelope and mating space |
| [Hirose DF23 product](https://www.hirose.com/en/product/p/CL0688-0804-6-92) | Exact mating plug and contact rating |
| [Hirose FH28H product](https://www.hirose.com/en/product/p/CL0586-1805-3-05); `Hirose_FH28_catalog.pdf` | 80-way land pattern, paste apertures, pin numbering, cable end and envelope |
| [Arduino GIGA CAD](https://docs.arduino.cc/static/5927a4ebbe3f363ebd68e7c50de5e0af/ABX00063-cad-files.zip) | Actual header contact positions, mounting holes and outline; extracted in `giga-geometry.json` |
| [Arduino GIGA STEP](https://docs.arduino.cc/resources/models/ABX00063-step.zip) | `libraries/3dmodels/GIGA_R1_WIFI.step` and mating assembly |
| [Arduino schematic](https://docs.arduino.cc/resources/schematics/ABX00063-schematics.pdf), copied as `GIGA_schematic.pdf` | Signal identity, 3.3 V rail and existing I2C4 pull-ups |
| [Arduino full pinout](https://docs.arduino.cc/resources/pinouts/ABX00063-full-pinout.pdf) | Arduino labels and MCU pin names |
| [TI SN74AXC8T245](https://www.ti.com/lit/ds/symlink/sn74axc8t245.pdf), local copy | Supply isolation, opposite DIR1/DIR2 levels, pinout and propagation limits |
| [TI SN74AXC1T45](https://www.ti.com/lit/ds/symlink/sn74axc1t45.pdf), local copy | Returning interrupt translation and power-off behavior |
| [TI TCA9517A](https://www.ti.com/lit/ds/symlink/tca9517a.pdf), local copy | A/B bus assignment, low-level offset and power-off behavior |
| [TI TPS26600](https://www.ti.com/lit/ds/symlink/tps2660.pdf), `TPS2660.pdf` | Reverse polarity/current protection, RTN isolation, threshold dividers, current limit, slew and PWP0016A land pattern |
| [TRACO TSR 1](https://www.tracopower.com/products/tsr1.pdf), `TRACO_TSR1.pdf` | TSR 1-24120 input/output limits and mechanical pinout |
| [TRACO TSR 2](https://www.tracopower.com/products/tsr2.pdf), `TRACO_TSR2.pdf` | TSR 2-2433 regulation, thermal coefficient, load limits and mechanical pinout |
| [Bourns SF-1206S](https://www.bourns.com/docs/product-datasheets/sf-1206s.pdf), `Bourns_SF1206S.pdf` | 2 A and 0.8 A 63 V time-lag fuses and pad dimensions |
| [Littelfuse SMBJ](https://m.littelfuse.com/~/media/electronics/datasheets/tvs_diodes/littelfuse_tvs_diode_smbj_datasheet.pdf.pdf) | SMBJ33CA bidirectional TVS, 33 V standoff / 53.3 V rated clamp |
| [Diodes B360A](https://www.diodes.com/datasheet/download/B360A.pdf) | 60 V Schottky polarity, SMA package and forward drop |
| [Phoenix 1751248](https://www.phoenixcontact.com/en-dk/products/pcb-terminal-block-mkds-1-2-35-1751248) | 3.5 mm terminal pitch, body and lead length |
| [Samtec socket catalogue](https://suddendocs.samtec.com/catalog_english/ssw_th.pdf), `Samtec_SSQ.pdf` | SSQ body dimensions and distinct -03 / -04 tail lengths |
| ST DS12930 Rev 3 Table 105, repository `docs/arduino_giga_r1_wifi/DS_stm32h747xi.pdf` | Conditional LTDC timing budget and its characterization conditions |
| [PCBWay capabilities](https://www.pcbway.com/capabilities.html) | Layer count, materials, thickness, copper, minimum geometry and silkscreen |
| [PCBWay assembly requirements](https://www.pcbway.com/assembly-faq.html) | Gerber, BOM, centroid and assembly deliverables |

The retained FH28 PDF is a manufacturer-authored catalogue obtained from the
[TME mirror](https://www.tme.eu/Document/6a1361134ed9e0f8d0fab8cdb9d25f31/FH28.pdf)
when the manufacturer's download endpoint was unavailable. It is not a reseller's
replacement footprint. The PCB pattern was checked against the dimensioned drawing.

## Library attribution

Arduino's original CAD is CC-BY-SA 4.0; its original notice is
`Arduino-CAD-LICENSE.txt`, and the complete license is `CC-BY-SA-4.0.txt`.
The extracted geometry and derived GIGA mating models retain that attribution.
The official STEP is unmodified. `GIGA_outer_socket_envelopes.step` adds nominal
8.5 mm socket bodies absent from that model; those bodies are explicitly not
manufacturer production models.

Standard resistor, capacitor, fuse, SOT-23, TSSOP and VSSOP footprints and STEP
models originate from the KiCad library contributors, installed KiCad libraries
on this workstation. Their license is CC-BY-SA 4.0 with KiCad's electronic-design
exception; see `KiCad-LIBRARY-LICENSE.txt` and the full CC license. Local changes
replace model paths, suppress crowded reference silkscreen and set project
manufacturing clearances. The TPS26600 land pattern uses TI’s PWP0016A recommended 3.3 mm exposed pad and 1.5 × 0.45 mm leads. Its 3D envelope and the display converter envelope follow the retained drawings; they are not production-tooling models.

Hirose, terminal and GIGA stacking-header footprints and simplified mechanical
envelopes were constructed for this project from the cited drawings. They are
not manufacturer-certified footprints or models. First-board fit and electrical
qualification are recorded separately from the digital release checks.

## Tools

KiCad 10.0.6 generated and checked the schematic/PCB and manufacturing exports.
FreeRouting was a routing aid; the reviewed KiCad PCB is authoritative. FreeCAD
1.1.3 generated envelope models and the mating assembly. Gerbonara 1.6.3 parsed
the exported Gerbers and drills independently of KiCad; Inkscape rendered those
parsed layers. Project-specific audits are retained in `tools/`.

The manufacturer-authored TRACO PDFs were obtained from the
[datasheets.com TSR1 mirror](https://www.datasheets.com/traco-power/tsr1-24120/datasheet.pdf)
and [TSR2 mirror](https://datasheet.octopart.com/TSR2-2433-Tracopower-datasheet-140787542.pdf)
when the manufacturer's download endpoint rejected automated access. The retained
TSR2 specification is dated 2020-04-27; the installed KiCad footprint has a larger
courtyard around the same 14 × 7.5 mm nominal body. Supply ratings and tolerance
budgets use the retained manufacturer specification, including ±0.02%/K.
