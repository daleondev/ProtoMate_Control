# Component purchasing list — display E / GIGA D

Quantities are for **one display adapter revision E and one GIGA adapter revision D**:
158 fitted components, 39 original BOM part numbers. The DC jack is already owned.
After the substitutions below and consolidating the 100 nF capacitors, Mouser
procurement needs 37 distinct parts: **36 with indexed stock evidence (156 pieces)**
and **one backordered fuse (1 piece)**. The FFC cable is an additional assembly item.

**This is not yet a fully in-stock order.** GIGA F1 is backordered, and no exact
Mouser cable satisfying our specification was verified. Stock evidence was checked
on **2026-10-22**, using indexed Mouser pages; their crawl ages range from days to
months. Direct live inventory confirmation was unavailable. Quantities below are
historical page observations, not reserved stock. In particular, the AXC8T245
product/category pages disagree; check it in the German delivery basket before paying.

- [Complete component list](components.csv), including references, original and purchase MPNs, packages, quantities and source URLs.
- [Display-only list](display-adapter-components.csv).
- [GIGA-only list](giga-adapter-components.csv).
- [Mouser bulk-import CSV](mouser-order.csv): 36 consolidated lines with indexed stock; **excludes F1, the owned jack and accessories**.
- [Separate backorder CSV](mouser-backorder.csv): GIGA F1, `652-SF-1206S200-2`, quantity 1.
- [Dated sourcing evidence](sourcing.json), including stock-page ages and substitution notes.

Import the desired CSV using Mouser's BOM upload/bulk-entry facility and map its
two columns to **Mouser Part Number** and **Quantity**. Select Germany as the
delivery country and review availability, quantities, MPNs and prices. Use cut
tape for SMT parts and individual through-hole parts; full reels/MouseReels are
unnecessary. The quantities are exact fitted quantities, without soldering spares.
For hand soldering, spare small passives and one spare of each fine-pitch IC/connector
are sensible additions. Do not substitute the precision 0.1% resistors with 1% parts.

The source KiCad projects and released manufacturing BOMs have **not** been changed.
This procurement list explicitly proposes the alternatives below for the same PCB
footprints. It does not supersede the release's pending electrical/physical tests.
The original BOM MPN remains in every CSV row so assembly substitutions are traceable.

## Display adapter — revision E

42 fitted components.

| References | Qty | Purchase MPN | Mouser order number | Observed availability |
|---|---:|---|---|---|
| J1 | 1 | `DF23C-50DP-0.5V(92)` | [798-DF23C50DP05V92](https://www.mouser.com/en/ProductDetail/Hirose-Connector/DF23C-50DP-0.5V92?qs=nSryOFbzj8IvGUDyjNwvaw%3D%3D) | Stock indexed; 1,575 (2 weeks ago) |
| J2 | 1 | `FH28H-80S-0.5SH(05)` | [798-FH28H80S05SH05](https://www.mouser.ca/ProductDetail/Hirose-Connector/FH28H-80S-0.5SH05?qs=nSryOFbzj8LLNK%252BjXgUxmQ%3D%3D) | Stock indexed; 822 (3 months ago) |
| U1 | 1 | `TSR 2-2433` | [495-TSR2-2433](https://eu.mouser.com/en/ProductDetail/TRACO-Power/TSR-2-2433?qs=NtE2QagKf6Qer8edccxn%2FA%3D%3D) | Stock indexed; 6,134 (3 weeks ago) |
| C9, C11 | 2 | `MCJCU32MLB7106KPPDTJ` † | [963-U32MLB7106KPPDTJ](https://my.mouser.com/en/ProductDetail/TAIYO-YUDEN/MCJCU32MLB7106KPPDTJ?qs=sGAEpiMZZMuMW9TJLBQkXugdD5SwFUVxUr871sYQlbI%3D) | Stock indexed; 1,535 (2 months ago) |
| C10 | 1 | `C0603C104K5RACTU` † | [80-C0603C104K5R](https://cz.mouser.com/en/ProductDetail/KEMET/C0603C104K5RACTU?qs=l5k%252BbMnNDkkVcnZPSAaaiQ%3D%3D) | Stock indexed; 3,941,024 (last week) |
| C1–C4, C12–C14 | 7 | `GRM32ER61A476KE20L` | [81-GRM32ER61A476KE20](https://my.mouser.com/en/ProductDetail/Murata-Electronics/GRM32ER61A476KE20L?qs=sGAEpiMZZMt7gvpyg0xT8kkEs7CruhcJf4%252BbaFr4BQM%3D) | Stock; EOL; 6,202 (last month) |
| C5–C6 | 2 | `C0805C106K8PACTU` † | [80-C0805C106K8P](https://eu.mouser.com/en/ProductDetail/KEMET/C0805C106K8PACTU?qs=Zpyg6gQUYs39C21BeavaKA%3D%3D) | Stock indexed; 235,800 (last week) |
| C7–C8, C16–C18 | 5 | `C0603C104K5RACTU` † | [80-C0603C104K5R](https://cz.mouser.com/en/ProductDetail/KEMET/C0603C104K5RACTU?qs=l5k%252BbMnNDkkVcnZPSAaaiQ%3D%3D) | Stock indexed; 3,941,024 (last week) |
| R2–R5, R8 | 5 | `RC0402FR-0747KL` | [603-RC0402FR-0747KL](https://eu.mouser.com/en/ProductDetail/YAGEO/RC0402FR-0747KL?qs=V1yeUXFNrklslMUO6Zhx5g%3D%3D) | Stock indexed; 986,351 (last month) |
| R6–R7 | 2 | `RC0402FR-074K7L` | [603-RC0402FR-074K7L](https://www.mouser.com/en/ProductDetail/YAGEO/RC0402FR-074K7L?qs=V1yeUXFNrkn%2FmCXC8N8TTA%3D%3D) | Stock indexed; 5,902,658 (last week) |
| U2 | 1 | `TSR 1-2450` | [495-TSR-1-2450](https://eu.mouser.com/en/ProductDetail/TRACO-Power/TSR-1-2450?qs=ckJk83FOD0XFKqda0Mzkgw%3D%3D) | Stock indexed; 13,356 (3 weeks ago) |
| U3 | 1 | `TPS7A9101DSKT` † | [595-TPS7A9101DSKT](https://eu.mouser.com/en/ProductDetail/Texas-Instruments/TPS7A9101DSKT?qs=34RfhUjJmKdmPKJyeIMDWg%3D%3D) | Stock indexed; 105 (3 weeks ago) |
| C15 | 1 | `C0603C473K5RACTU` † | [80-C0603C473K5R](https://www.mouser.com/en/ProductDetail/KEMET/C0603C473K5RACTU?qs=CDC5R06TEgYvy7Nwr10FTA%3D%3D) | Stock indexed; 17,294 (last month) |
| R9 | 1 | `TNPW060331K2BEEN` † | [71-TNPW060331K2BEEN](https://www.mouser.tw/en/ProductDetail/Vishay/TNPW060331K2BEEN?qs=sGAEpiMZZMvdGkrng054twbPyJJnNTBKZ%252B4sXEeYlOM%3D) | Stock indexed; 280 (last month) |
| R10 | 1 | `RT0603BRD0710KL` | [603-RT0603BRD0710KL](https://eu.mouser.com/en/ProductDetail/YAGEO/RT0603BRD0710KL?qs=Br90QIIERxSXuxEMyECqGA%3D%3D) | Stock indexed; 69,151 (last week) |
| R11–R15 | 5 | `RC0603FR-0710KL` | [603-RC0603FR-0710KL](https://www.mouser.com/en/ProductDetail/YAGEO/RC0603FR-0710KL?qs=grNVn54RoB%252B3GtjbJj3wJQ%3D%3D) | Stock indexed; 1,640,807 (last week) |
| U4, U6 | 2 | `TPS3808G33DBVR` | [595-TPS3808G33DBVR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/TPS3808G33DBVR?qs=zwk82MlA57YGs7Rey7Yv1A%3D%3D) | Stock indexed; 18,673 (last month) |
| R16–R17 | 2 | `RC0402FR-07100KL` | [603-RC0402FR-07100KL](https://www.mouser.com/en/ProductDetail/YAGEO/RC0402FR-07100KL?qs=mnq%2FyloZIXzTlUn7JAHSWg%3D%3D) | Stock indexed; 6,394,487 (last month) |
| U5 | 1 | `SN74LVC08APW` † | [595-SN74LVC08APW](https://www.mouser.com/en/ProductDetail/Texas-Instruments/SN74LVC08APW?qs=RZS%252BpEsUn7pxqABTZcJnVg%3D%3D) | Stock indexed; 252 (last month) |

## GIGA adapter — revision D

116 fitted components. `Qty` is the fitted quantity; J10 is already owned.

| References | Qty | Purchase MPN | Mouser order number | Observed availability |
|---|---:|---|---|---|
| J1 | 1 | `FH28H-80S-0.5SH(05)` | [798-FH28H80S05SH05](https://www.mouser.ca/ProductDetail/Hirose-Connector/FH28H-80S-0.5SH05?qs=nSryOFbzj8LLNK%252BjXgUxmQ%3D%3D) | Stock indexed; 822 (3 months ago) |
| U1–U4 | 4 | `SN74AXC8T245PWR` | [595-SN74AXC8T245PWR](https://gr.mouser.com/en/c/semiconductors/logic-ics/?mounting+style=SMD%2FSMT&number+of+channels=8+Channel&package+%2F+case=TSSOP-24) | Conflicting stock snapshots; 4,862 (last week) |
| R1–R32 | 32 | `RC0402FR-0733RL` | [603-RC0402FR-0733RL](https://eu.mouser.com/en/ProductDetail/YAGEO/RC0402FR-0733RL?qs=V1yeUXFNrkk7PcMc%252BKdghg%3D%3D) | Stock indexed; 1,477,025 (last month) |
| R33–R65 | 33 | `RC0402FR-0747KL` | [603-RC0402FR-0747KL](https://eu.mouser.com/en/ProductDetail/YAGEO/RC0402FR-0747KL?qs=V1yeUXFNrklslMUO6Zhx5g%3D%3D) | Stock indexed; 986,351 (last month) |
| C1–C12, C20–C21 | 14 | `C0603C104K5RACTU` † | [80-C0603C104K5R](https://cz.mouser.com/en/ProductDetail/KEMET/C0603C104K5RACTU?qs=l5k%252BbMnNDkkVcnZPSAaaiQ%3D%3D) | Stock indexed; 3,941,024 (last week) |
| U5, U9 | 2 | `SN74AXC1T45DBVR` | [595-SN74AXC1T45DBVR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/SN74AXC1T45DBVR?qs=W0yvOO0ixfHY47jVlAKDLA%3D%3D) | Stock indexed; 6,687 (last month) |
| U6 | 1 | `TCA9517ADGKR` | [595-TCA9517ADGKR](https://eu.mouser.com/en/ProductDetail/Texas-Instruments/TCA9517ADGKR?qs=%252BrH4t%252BeVZ2M4pVnwXR4oNA%3D%3D) | Stock indexed; 254 (3 weeks ago) |
| C13–C14 | 2 | `C0805C106K8PACTU` † | [80-C0805C106K8P](https://eu.mouser.com/en/ProductDetail/KEMET/C0805C106K8PACTU?qs=Zpyg6gQUYs39C21BeavaKA%3D%3D) | Stock indexed; 235,800 (last week) |
| J10 | 1 | `DC-005 / B07J4Z8B2T` | — | Already owned |
| F1 | 1 | `SF-1206S200-2` | [652-SF-1206S200-2](https://www.mouser.com/en/ProductDetail/Bourns/SF-1206S200-2?qs=GIQXYiEeHGCJory%252B0WHOSw%3D%3D) | Backorder; 0 (6 days ago) |
| D1 | 1 | `SMBJ33CA-13-F` † | [621-SMBJ33CA-13-F](https://www.mouser.com/en/ProductDetail/Diodes-Incorporated/SMBJ33CA-13-F?qs=gaDBXWSqsDCWsx7MwPOf0g%3D%3D) | Stock indexed; 64,961 (last month) |
| C15 | 1 | `C1206C105K1RACTU` † | [80-C1206C105K1R](https://www.mouser.se/en/ProductDetail/KEMET/C1206C105K1RACTU?qs=sGAEpiMZZMvsSlwiRhF8qoXfCmt4ymbCzzXhwOByAo0F9nx%252Bkx%252B2ag%3D%3D) | Stock indexed; 41,000 (last month) |
| U7 | 1 | `TPS26600PWPR` | [595-TPS26600PWPR](https://www.mouser.com/en/ProductDetail/Texas-Instruments/TPS26600PWPR?qs=Bho%2FbeBaDExFVQBFO8fB%2FQ%3D%3D) | Stock indexed; 69 (last month) |
| R66 | 1 | `RC0603FR-07143KL` | [603-RC0603FR-07143KL](https://www.mouser.tw/en/ProductDetail/YAGEO/RC0603FR-07143KL?qs=IuGqVx9wL0I9yevS9IvNIg%3D%3D) | Stock indexed; 21,769 (last week) |
| R67, R69, R71 | 3 | `RC0603FR-0710KL` | [603-RC0603FR-0710KL](https://www.mouser.com/en/ProductDetail/YAGEO/RC0603FR-0710KL?qs=grNVn54RoB%252B3GtjbJj3wJQ%3D%3D) | Stock indexed; 1,640,807 (last week) |
| R68 | 1 | `RC0603FR-07226KL` | [603-RC0603FR-07226KL](https://www.mouser.com/en/ProductDetail/YAGEO/RC0603FR-07226KL?qs=0yPvnVz5%2Fmr%2FHz6oBKp3LQ%3D%3D) | Stock indexed; 141,223 (last month) |
| R70 | 1 | `RC0603FR-078K06L` | [603-RC0603FR-078K06L](https://eu.mouser.com/en/ProductDetail/YAGEO/RC0603FR-078K06L?qs=2cAdsCoAWREXE%2FuVdbo5eQ%3D%3D) | Stock indexed; 47,045 (last week) |
| C16 | 1 | `C0603C473K5RACTU` † | [80-C0603C473K5R](https://www.mouser.com/en/ProductDetail/KEMET/C0603C473K5RACTU?qs=CDC5R06TEgYvy7Nwr10FTA%3D%3D) | Stock indexed; 17,294 (last month) |
| C17–C18 | 2 | `MCJCU32MLB7106KPPDTJ` † | [963-U32MLB7106KPPDTJ](https://my.mouser.com/en/ProductDetail/TAIYO-YUDEN/MCJCU32MLB7106KPPDTJ?qs=sGAEpiMZZMuMW9TJLBQkXugdD5SwFUVxUr871sYQlbI%3D) | Stock indexed; 1,535 (2 months ago) |
| D2–D3 | 2 | `B360A-13-F` | [621-B360A-F](https://au.mouser.com/ProductDetail/Diodes-Incorporated/B360A-13-F?qs=sVoUGXYOGv3xoCWNHVb4bA%3D%3D) | Stock indexed; 112,297 (3 months ago) |
| F2 | 1 | `SF-1206S080-2` | [652-SF-1206S080-2](https://www.mouser.com/pdfdocs/bourns_singlfuse_short_form-2.pdf?srsltid=AfmBOoqnInMhj144LjRkX9XTdRx5xJ6Ayb6MES2Zdy1csFnhwGkjeilB) | Stock indexed; 3,718 (unspecified) |
| U8 | 1 | `TSR 1-24120` | [495-TSR-1-24120](https://www.mouser.com/en/ProductDetail/TRACO-Power/TSR-1-24120?qs=ckJk83FOD0XFyuKFnkiJHw%3D%3D) | Stock indexed; 5,150 (last month) |
| C19 | 1 | `08053D106KAT2A` † | [581-08053D106KAT2A](https://www.mouser.com/en/ProductDetail/KYOCERA-AVX/08053D106KAT2A?qs=RC%2FTMc7rOJbkID75Q0PA3w%3D%3D) | Stock indexed; 37 (last month) |
| J2 | 1 | `SSQ-110-03-G-S` | [200-SSQ11003GS](https://www.mouser.com/ProductDetail/Samtec/SSQ-110-03-G-S?qs=0u6pJ%2FESxi2TyhdgZpByhg%3D%3D) | Stock indexed; 1,271 (4 months ago) |
| J3 | 1 | `SSQ-108-03-G-S` | [200-SSQ10803GS](https://eu.mouser.com/ProductDetail/Samtec/SSQ-108-03-G-S?qs=92ilVni64gxESeLoQPf0Dw%3D%3D) | Stock indexed; 2,289 (5 months ago) |
| J4 | 1 | `SSQ-108-03-G-S` | [200-SSQ10803GS](https://eu.mouser.com/ProductDetail/Samtec/SSQ-108-03-G-S?qs=92ilVni64gxESeLoQPf0Dw%3D%3D) | Stock indexed; 2,289 (5 months ago) |
| J5 | 1 | `SSQ-108-03-G-S` | [200-SSQ10803GS](https://eu.mouser.com/ProductDetail/Samtec/SSQ-108-03-G-S?qs=92ilVni64gxESeLoQPf0Dw%3D%3D) | Stock indexed; 2,289 (5 months ago) |
| J6 | 1 | `SSQ-108-03-G-S` | [200-SSQ10803GS](https://eu.mouser.com/ProductDetail/Samtec/SSQ-108-03-G-S?qs=92ilVni64gxESeLoQPf0Dw%3D%3D) | Stock indexed; 2,289 (5 months ago) |
| J7 | 1 | `SSQ-108-03-G-S` | [200-SSQ10803GS](https://eu.mouser.com/ProductDetail/Samtec/SSQ-108-03-G-S?qs=92ilVni64gxESeLoQPf0Dw%3D%3D) | Stock indexed; 2,289 (5 months ago) |
| J8 | 1 | `SSQ-118-03-G-D` | [200-SSQ11803GD](https://www.mouser.com/en/c/connectors/?number+of+positions=36+Position&product=Headers&type=Socket) | Stock indexed; 80 (last week) |
| J9 | 1 | `SSQ-112-04-G-D` | [200-SSQ11204GD](https://www.mouser.com/ProductDetail/Samtec/SSQ-112-04-G-D?qs=rU5fayqh%252BE0yGPselSDHYw%3D%3D) | Stock indexed; 261 (3 months ago) |

## Purchasing substitutions (†)

The IC changes are carrier/packaging changes only. The other replacements preserve
the nominal value, tolerance, dielectric/function and PCB package, with equal or
higher voltage rating. They are procurement proposals, not measured first-board
qualification. In particular, equal nominal MLCC values do not establish identical
DC-bias capacitance or transient response; retain the release's supply/ramp/load-step
tests. No bulk 47 µF capacitor substitution is proposed.

| Released BOM MPN | Purchase MPN | Reason and compatibility |
|---|---|---|
| `GRM32ER71H106KA12L` | `MCJCU32MLB7106KPPDTJ` | Replacement: 10 uF +/-10%, 50 V, X7R, 1210, soft termination; 2.5 +/-0.3 mm height. Original unavailable/region-restricted. |
| `GRM188R71H104KA93D` | `C0603C104K5RACTU` | Replacement for obsolete original: 100 nF +/-10%, 50 V, X7R, 0603. |
| `GRM21BR61A106KE19L` | `C0805C106K8PACTU` | Replacement for obsolete original: 10 uF +/-10%, 10 V, X5R, 0805; 1.25 +/-0.15 mm height. |
| `GRM188R71C104KA01D` | `C0603C104K5RACTU` | Replacement for EOL original: 100 nF +/-10%, X7R, 0603; voltage rating increased from 16 V to 50 V. Consolidated with display C10. |
| `TPS7A9101DSKR` | `TPS7A9101DSKT` | Packaging-only replacement: same DSK-10 chip/pinout; small-reel suffix T instead of R. Exposed pad must be soldered. |
| `GRM188R71C473KA01D` | `C0603C473K5RACTU` | Replacement: 47 nF +/-10%, X7R, 0603; 50 V rating exceeds original 16 V. |
| `RT0603BRD0731K2L` | `TNPW060331K2BEEN` | Replacement: 31.2 kohm, 0.1%, 25 ppm/K, 0603. Preserve precision LDO ratio; original backordered. |
| `SN74LVC08APWR` | `SN74LVC08APW` | Packaging-only replacement: same TSSOP-14 PW chip, supplied in tube; original reel variant backordered. |
| `SMBJ33CA` | `SMBJ33CA-13-F` | Replacement for backordered Littelfuse: bidirectional 33 V, 600 W TVS in SMB; 53.3 V maximum specified clamp. Do not use unidirectional A. |
| `GRM31CR72A105KA01L` | `C1206C105K1RACTU` | Replacement: 1 uF +/-10%, 100 V, X7R, 1206; original backordered. |
| `GRM21BR61E106KA73L` | `08053D106KAT2A` | Replacement: 10 uF +/-10%, 25 V, X5R, 0805; original backordered. |

The following manufacturer information supports these choices:

- [TI SN74LVC08A package options](https://www.ti.com/ods/sysadd/oa/symlink/sn54lvc08a_oa.pdf): PW and PWR use the same 14-pin PW package, supplied in tubes versus reels.
- [TI TPS7A91 datasheet](https://www.ti.com/lit/ds/symlink/tps7a91.pdf): DSKT and DSKR use the same DSK-10 device/package; only reel quantity differs.
- [TAIYO YUDEN 10 µF capacitor](https://ds.yuden.co.jp/TYCOMPAS/ap/sm/detail?pn=MCJCU32MLB7106KPPDTJ&u=M): 50 V, ±10%, X7R, 1210; maximum body height 2.8 mm. All affected capacitors are on the component side, away from the underside display mating gap.
- [KEMET 100 nF](https://search.kemet.com/component-documentation/download/specsheet/C0603C104K5RACTU), [10 µF / 10 V](https://search.kemet.com/download/specsheet/C0805C106K8PACTU) and [1 µF / 100 V](https://search.kemet.com/component-documentation/download/specsheet/C1206C105K1RACTU): values, dielectric, voltage and body sizes.
- [Diodes SMBJ33CA](https://www.diodes.com/part/view/SMBJ33CA): bidirectional 33 V standoff, 600 W, SMB, 53.3 V specified maximum clamp; retain the existing surge-qualification limitation.

## Items requiring attention before a complete order

1. **GIGA F1:** `SF-1206S200-2`, Mouser `652-SF-1206S200-2`, has zero stock in the most recent indexed product page. An incoming shipment is listed for **2026-11-30**; this is not a confirmed delivery date. It is separated into the backorder CSV. Do not use F2's 0.8 A fuse or a resettable PTC here.
2. **GIGA U1–U4:** the newer Mouser category page lists 4,862 `SN74AXC8T245PWR`, while the older product page lists zero. This is conflicting cached evidence, so availability remains provisional until basket validation. The alternate `SN74AXCH8T245` is not the selected device.
3. **Display bulk capacitors:** seven `GRM32ER61A476KE20L` remain listed in stock but are EOL. The list retains the electrically reviewed part for this prototype instead of silently changing its load-step behavior.
4. **FFC cable:** one required, outside the PCB BOM. No exact Mouser MPN was verified. Use the [cable specification](../cable-specification.pdf): 80 conductors, 0.5 mm pitch, ≤100 mm, 0.30 ±0.05 mm mating thickness, same-side exposed contacts (Type A), straight sides, gold contacts, ≥60 V and ≥0.5 A per conductor, copper cross-section ≥0.0105 mm². The FH28 termination/stiffener drawing must also match. The surfaced 102 mm / 0.3 A Molex cable is **not** an approved replacement.

An in-stock Littelfuse `0468002.NR` was investigated for F1 but intentionally not
added to the order: it has a different clearing-time specification and nominal
melting I²t (0.506 A²s versus 0.339 A²s). Matching 2 A/63 V/1206 alone does not make
fuses equivalent. A change would need a protection-coordination review. Sources:
[Bourns SF-1206S](https://www.bourns.com/docs/product-datasheets/sf-1206s.pdf),
[Littelfuse 468](https://www.littelfuse.com/assetdocs/fuse-468-datasheet?assetguid=6b7857dc-f79c-4aae-8bcc-a9ef11ddef09).

## Assembly items outside the soldered component BOM

| Item | Quantity | Sourcing / dimensions |
|---|---:|---|
| DC-005 input jack | 1 | **Already owned**; included at GIGA J10 above, buy quantity zero. |
| Specified 80-way FFC cable | 1 | Unresolved Mouser source; see cable requirements above. |
| Arduino GIGA R1 WiFi and Smartwin SMMH0701024600S-C20 | 1 each | Existing system hardware; not included in the board-parts order. |
| Regulated 24 V supply and centre-positive 5.5 × 2.1 mm plug | 1 set | Existing system supply, ≥2 A capacity; no new supply assumed. |
| GIGA insulating supports / fasteners | 1 set | 13 mm shield-underside clearance; use the assembly drawing, including the two lower M2 mounts. Upper alignment holes are not ordinary clamped mounts. |
| Display supports / strain relief | 1 set | Four M2 mounting positions; enclosure-dependent screw lengths. Keep load off the panel flex and DF23 connector. |
| Test points, mounting holes, copper markers | — | PCB features; no additional electronic parts to buy. |

Samtec tail lengths are part of the mechanical design: J2–J8 use **-03 (10 mm)**,
J9 uses **-04 (14.83 mm)**. Generic stacking-header kits are not a qualified replacement.
Physical fit, soldering and powered behavior remain the first-board checks already
identified by the manufacturing release.

## Traceability

The lists were reconciled to every fitted reference in the release BOMs. Exact
source hashes are recorded below; if the PCB/BOM changes, regenerate/reconcile the
purchase list before reuse. Stock pages should be rechecked at order time.

- `manufacturing/display-adapter-rev-E/bom-all.csv`: `d5a7767cd17b775a20798eca16c5544435ac585152db53b07fc0ae5ac9c478a6`
- `manufacturing/giga-adapter-rev-D/bom-all.csv`: `45707a749848babcfde0896e527d6be066bae138784e494b6d7b8103ac199821`
