#!/usr/bin/env python3
"""Check the native KiCad project and refresh its review/assembly exports.

Requires KiCad 10's CLI and pcbnew Python module, plus Poppler's pdfunite.
Never generates or overwrites the source schematic, layout, or wire schedule.
"""

import argparse
import collections
import csv
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import pcbnew as pcb


ROOT = Path(__file__).resolve().parents[1]
ASSEMBLY = ROOT / "assembly"
EXPORTS = ROOT / "exports"
BOARD = ROOT / "ProtoMate.kicad_pcb"
SCHEMATIC = ROOT / "ProtoMate.kicad_sch"
MODULE_MOUNTS = {"A3": "J105", "A4": "J106"}
MOUNTED_MODULES = {"U1": "J103", **MODULE_MOUNTS}
GRID_COLUMNS, GRID_ROWS = 39, 48
GRID_ORIGIN, GRID_PITCH = 50.8, 2.54
BOARD_LEFT = BOARD_TOP = round(GRID_ORIGIN - GRID_PITCH / 2, 6)
BOARD_RIGHT = round(BOARD_LEFT + GRID_COLUMNS * GRID_PITCH, 6)
BOARD_BOTTOM = round(BOARD_TOP + GRID_ROWS * GRID_PITCH, 6)
# Compatibility with KiCad 10's SWIG wrappers on Python 3.14.
pcb.SwigPyIterator.next = pcb.SwigPyIterator.__next__


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True, **kwargs)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def connected_name(name):
    return "" if name.startswith("unconnected-") else name


def read_csv(name):
    with (ASSEMBLY / name).open(newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv(name, headings, rows):
    with (ASSEMBLY / name).open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(headings)
        writer.writerows(rows)


def natural(text):
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", text)]


def hole(item):
    pos = item.GetPosition()
    x, y = ((pcb.ToMM(n) - GRID_ORIGIN) / GRID_PITCH for n in (pos.x, pos.y))
    require(abs(x - round(x)) < 1e-6 and abs(y - round(y)) < 1e-6,
            f"Off-grid position: {pos}")
    require(0 <= round(x) < GRID_COLUMNS and 0 <= round(y) < GRID_ROWS,
            "Position outside the 39 x 48 hole field A1:AV39")
    row, letters = round(y) + 1, ""
    while row:
        row, remainder = divmod(row - 1, 26)
        letters = chr(65 + remainder) + letters
    return letters + str(round(x) + 1)


def verify(netlist, board):
    edges = [item for item in board.GetDrawings() if item.GetLayer() == pcb.Edge_Cuts]
    corners = {(round(pcb.ToMM(v.x), 6), round(pcb.ToMM(v.y), 6))
               for edge in edges for v in (edge.GetStart(), edge.GetEnd())}
    require(len(edges) == 4 and corners == {
        (BOARD_LEFT, BOARD_TOP), (BOARD_RIGHT, BOARD_TOP),
        (BOARD_RIGHT, BOARD_BOTTOM), (BOARD_LEFT, BOARD_BOTTOM)},
        "Board outline differs from the 39 x 48 grid with half-pitch margins")
    grid = [item for item in board.GetDrawings()
            if isinstance(item, pcb.PCB_SHAPE) and item.GetLayer() == pcb.Dwgs_User
            and item.GetShape() == pcb.SHAPE_T_CIRCLE]
    require(len(grid) == GRID_COLUMNS * GRID_ROWS and
            len({hole(item) for item in grid}) == GRID_COLUMNS * GRID_ROWS,
            "Drawing must show all 1872 holes exactly once")
    document = ET.parse(netlist).getroot()
    components = {c.get("ref"): c for c in document.findall("./components/comp")}
    onboard = {ref: c for ref, c in components.items()
               if c.find("property[@name='exclude_from_board']") is None}
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    require(onboard.keys() == footprints.keys(), "Schematic/board component sets differ")
    source = {(n.get("ref"), n.get("pin")): net.get("name")
              for net in document.findall("./nets/net") for n in net.findall("node")}
    # The external controller harness must agree with the firmware source of
    # truth as well as with the perfboard. ERC alone cannot catch a stale GPIO.
    ioc = dict(line.split("=", 1) for line in
               (ROOT.parent / "external/CubeMX/CubeMX.ioc").read_text().splitlines()
               if "=" in line and not line.startswith("#"))
    controller_routes = {
        "M1_ALM": ("PF2", "CN9.17", "GPXTI2"),
        "M1_STEP": ("PA0", "CN10.29", "S_TIM2_CH1_ETR"),
        "M2_STEP": ("PB10", "CN10.32", "S_TIM2_CH3"),
        "M3_STEP": ("PB11", "CN10.34", "S_TIM2_CH4"),
        "M1_DIR": ("PE12", "CN10.26", "GPIO_Output"),
        "M2_DIR": ("PE13", "CN10.10", "GPIO_Output"),
        "M3_DIR": ("PE14", "CN10.8", "GPIO_Output"),
        "STEPPERS_EN_N": ("PE15", "CN10.30", "GPIO_Output"),
        "M1_ENC_A": (r"PB4\ (NJTRST)", "CN7.19", "S_TIM3_CH1"),
        "M1_ENC_B": ("PB5", "CN7.13", "S_TIM3_CH2"),
        "M1_ENC_Z": ("PB6", "CN12.17", "GPXTI6"),
        "M1_REF": ("PE7", "CN10.20", "GPXTI7"),
        "M2_REF": ("PE8", "CN10.18", "GPXTI8"),
        "M3_REF": ("PE10", "CN10.24", "GPXTI10"),
        "TMC_UART_TX": ("PD5", "CN9.6", "USART2_TX"),
        "TMC_UART_RX": ("PD6", "CN9.4", "USART2_RX"),
        "M2_INDEX": ("PD0", "CN9.25", "GPXTI0"),
        "M3_INDEX": ("PD1", "CN9.27", "GPXTI1"),
        "M2_DIAG": ("PD4", "CN9.8", "GPXTI4"),
        "M3_DIAG": ("PD3", "CN9.10", "GPXTI3"),
        "FLASH_CLK": ("PB2", "CN10.15", "QUADSPI_CLK"),
        "FLASH_CS_N": ("PG6", "CN10.13", "QUADSPI_BK1_NCS"),
        "FLASH_IO0": ("PD11", "CN10.23", "QUADSPI_BK1_IO0"),
        "FLASH_IO1": ("PD12", "CN10.21", "QUADSPI_BK1_IO1"),
        "FLASH_IO2": ("PE2", "CN10.25", "QUADSPI_BK1_IO2"),
        "FLASH_IO3": ("PD13", "CN10.19", "QUADSPI_BK1_IO3"),
        "SD_D0": ("PC8", "CN8.2", "SDMMC1_D0"),
        "SD_D1": ("PC9", "CN8.4", "SDMMC1_D1"),
        "SD_D2": ("PC10", "CN8.6", "SDMMC1_D2"),
        "SD_D3": ("PC11", "CN8.8", "SDMMC1_D3"),
        "SD_CLK": ("PC12", "CN8.10", "SDMMC1_CK"),
        "SD_CMD": ("PD2", "CN8.12", "SDMMC1_CMD"),
        "SD_CD_N": ("PG2", "CN8.14", "GPIO_Input"),
    }
    for net, (pin, contact, signal) in controller_routes.items():
        require(source.get(("A1", contact)) == net, f"Controller contact differs: {net}")
        require(ioc.get(f"{pin}.Signal") == signal,
                f"CubeMX GPIO/timer differs from wiring: {net}")
        if net.startswith(("M1_", "M2_", "M3_", "STEPPERS_", "TMC_")):
            require(ioc.get(f"{pin}.GPIO_Label") == net, f"CubeMX label differs: {net}")
    for pin in ("PE7", "PE8", "PE10"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLUP" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING_FALLING",
                f"NC reference input requires pull-up and both-edge EXTI: {pin}")
    require(ioc.get("PB6.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
            "Encoder index must use rising-edge EXTI after the polarity-preserving interface")
    require(ioc.get("PE15.PinState") == "GPIO_PIN_SET",
            "Shared active-low enable must start high (drivers disabled)")
    for pin in ("PD0", "PD1"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLDOWN" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING_FALLING",
                f"Driver INDEX requires pull-down and both-edge EXTI: {pin}")
    for pin in ("PD3", "PD4"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLDOWN" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
                f"Driver DIAG requires pull-down and rising-edge EXTI: {pin}")
    require(ioc.get("PF2.GPIO_PuPd") == "GPIO_PULLUP" and
            ioc.get("PF2.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
            "DM542T ALM requires pull-up and rising-edge EXTI2")
    for ref, pin, net in (("A2", "ALM+", "M1_ALM"), ("A2", "ALM-", "GND"),
                          ("R30", "1", "+3V3"), ("R30", "2", "M1_ALM"),
                          ("J114", "1", "M1_ALM"), ("J114", "2", "GND"),
                          ("J101", "4", "M1_ALM")):
        require(source.get((ref, pin)) == net, f"DM542T alarm connection differs: {ref}.{pin}")
    require(components["R30"].findtext("value") == "4k7", "ALM needs its 4.7k external 3.3 V pull-up")
    require(ioc.get("USART2.BaudRate") == "115200" and
            ioc.get("USART2.FIFOMode") == "FIFOMODE_ENABLE",
            "Shared driver UART must preserve 115200 baud and RX FIFO")
    for module, ms1 in (("A3", "GND"), ("A4", "+3V3")):
        require(source[module, "JP4.5"] == ms1 and source[module, "JP4.6"] == "GND" and
                source[module, "JP4.9"] == "TMC_UART_RX",
                f"Driver UART address/bus wiring differs: {module}")
    require(source["R29", "1"] == "TMC_UART_TX" and source["R29", "2"] == "TMC_UART_RX"
            and components["R29"].findtext("value") == "1k",
            "UART TX must join the shared RX bus through R29, 1k")
    pads = {(ref, p.GetNumber()): p for ref, f in footprints.items() for p in f.Pads()}
    require(len({hole(p) for p in pads.values()}) == len(pads), "Two leads occupy one hole")
    for ref, footprint in footprints.items():
        component = onboard[ref]
        # XML sheet paths omit the root UUID; native footprint paths include it.
        suffix = component.find("sheetpath").get("tstamps") + component.findtext("tstamps")
        require(footprint.GetPath().AsString().endswith(suffix), f"Symbol link differs: {ref}")
        require(footprint.GetFPIDAsString() == component.findtext("footprint"), f"Footprint differs: {ref}")
        require(footprint.GetValue() == component.findtext("value"), f"Value differs: {ref}")
        require(footprint.GetLayer() == pcb.F_Cu, f"Component is not on top: {ref}")
        expected = {pin: net for (r, pin), net in source.items() if r == ref}
        actual = {pin: p.GetNetname() for (r, pin), p in pads.items() if r == ref}
        require(expected == actual, f"Pin/net mismatch: {ref}")
    require({r for r, f in footprints.items() if f.IsDNP()} == {"R10", "R11", "R12"},
            "Unexpected DNP parts")
    power_positive = {("J103", "4"), ("J112", "1"), ("J105", "11"),
                      ("J106", "11"), ("C1", "1"), ("C2", "1")}
    require({key for key, p in pads.items() if p.GetNetname() == "+24V"} == power_positive,
            "24 V must connect only to the buck VIN, driver power pins and bulk capacitors")

    # TMC modules land on JP4 and JP1's two power contacts. Winding terminals
    # and the buck's barrel jack remain on the modules.
    for module, header in MOUNTED_MODULES.items():
        f = footprints[header]
        is_buck = module == "U1"
        footprint_name = ("QIQIAZI_XY3606_DirectMount" if is_buck else
                          "Adafruit_6121_JP4_Power_DirectMount")
        require(f.GetFPIDAsString() == f"ProtoMate_Perfboard:{footprint_name}",
                f"Missing module envelope: {module}")
        require(f.GetOrientationDegrees() == 0, f"Unexpected module rotation: {module}")
        contacts = ({1: "OUT+", 2: "OUT-", 3: "IN-", 4: "IN+"} if is_buck else
                    {**{n: f"JP4.{n}" for n in range(1, 11)}, 11: "JP1.6", 12: "JP1.5"})
        for pin, contact in contacts.items():
            require(connected_name(source[(module, contact)]) ==
                    connected_name(pads[header, str(pin)].GetNetname()),
                    f"Direct module header differs: {module} {contact}")
        if is_buck:
            require(source[("U1", "IN+")] == "+24V" and source[("U1", "IN-")] == "GND",
                    "Buck input supply differs from system schematic")
        x, y = (pcb.ToMM(v) for v in (f.GetPosition().x, f.GetPosition().y))
        body = ((x - 56.8, y - 8.5, x + 6.2, y + 18.5) if is_buck else
                (x - 1.905, y - 21.59, x + 24.765, y + 2.54))
        require(BOARD_LEFT <= body[0] and body[2] <= BOARD_RIGHT and
                BOARD_TOP <= body[1] and body[3] <= BOARD_BOTTOM,
                f"Module body extends beyond perfboard: {module}")
        for other, part in footprints.items():
            if other == header:
                continue
            bounds = part.GetBoundingBox(False, False)
            left, top, width, height = (pcb.ToMM(v) for v in
                                       (bounds.GetX(), bounds.GetY(), bounds.GetWidth(), bounds.GetHeight()))
            require(body[2] <= left or left + width <= body[0] or
                    body[3] <= top or top + height <= body[1],
                    f"Module body overlaps {other}: {module}")

    # Wires.csv is an assembly schedule, maintained alongside routing changes.
    wires = read_csv("Wires.csv")
    # Power branches must have dedicated copper returns, not a route through
    # the signal-ground tree. J112's two solder joints are the star points.
    power_links = set()
    for buck_pin, terminal_pin, module_pin in (("4", "1", "11"), ("3", "2", "12")):
        power_links.add(frozenset((("J103", buck_pin), ("J112", terminal_pin))))
        for cap, module in (("C1", "J105"), ("C2", "J106")):
            power_links.add(frozenset((("J112", terminal_pin), (cap, terminal_pin))))
            power_links.add(frozenset(((cap, terminal_pin), (module, module_pin))))
    power_links.add(frozenset((("J103", "3"), ("J103", "2"))))
    found_power_links = set()
    graph = collections.defaultdict(set)
    passages_used = collections.Counter()
    vias = {hole(t): t.GetNetname() for t in board.GetTracks() if isinstance(t, pcb.PCB_VIA)}
    occupied = {hole(p): p.GetNetname() for p in pads.values()}
    require(not (vias.keys() & occupied.keys()), "Wire passage overlaps a component lead")
    for wire in wires:
        a = wire["from_ref"], wire["from_pin"]
        b = wire["to_ref"], wire["to_pin"]
        link = frozenset((a, b))
        require(wire["wire_class"] == ("power" if link in power_links else "signal"),
                f"Wrong conductor class: {wire['wire']}")
        if link in power_links:
            require(wire["sides"] == "bottom wire", "Power branch must stay underneath")
            found_power_links.add(link)
        require(pads[a].GetNetname() == wire["net"] == pads[b].GetNetname(),
                f"Wrong wire net: {wire['wire']}")
        require(hole(pads[a]) == wire["from_hole"] and hole(pads[b]) == wire["to_hole"],
                f"Stale wire endpoint: {wire['wire']}")
        graph[a].add(b)
        graph[b].add(a)
        passages = list(filter(None, wire["side_change_holes"].split(", ")))
        require((wire["sides"] == "bottom wire" and not passages) or
                (wire["sides"] == "bottom wire + top jumper" and len(passages) == 2),
                f"Wire must stay underneath or use one crossover: {wire['wire']}")
        for passage in passages:
            require(vias.get(passage) == wire["net"],
                    f"Wrong wire passage: {wire['wire']}")
            passages_used[passage] += 1
    require(passages_used.keys() == vias.keys() and
            all(count == 1 for count in passages_used.values()),
            "Each crossover needs two dedicated free holes, without sharing wire passages")
    require(all(len(neighbours) <= 3 for neighbours in graph.values()),
            "More than three scheduled wire ends at one component solder joint")
    require(found_power_links == power_links, "Missing dedicated power branch")
    for module, resistor in (("J105", "R8"), ("J106", "R9")):
        require(graph[module, "2"] == {(module, "12"), (resistor, "2")},
                f"Driver logic ground must join its local power ground: {module}")
    netpads = collections.defaultdict(set)
    for key, pad in pads.items():
        if connected_name(pad.GetNetname()):
            netpads[pad.GetNetname()].add(key)
    for net, terminals in netpads.items():
        reached, todo = set(), [next(iter(terminals))]
        while todo:
            key = todo.pop()
            if key not in reached:
                reached.add(key)
                todo.extend(graph[key] - reached)
        require(reached == terminals, f"Wire schedule does not connect all pads: {net}")

    harness = read_csv("Harness.csv")
    header_pads = {key for key in pads if re.fullmatch(r"J\d+", key[0])}
    require({(r['Perfboard header'], r['Pin']) for r in harness} == header_pads,
            "Harness does not cover all header positions")
    for row in harness:
        key = row["Perfboard header"], row["Pin"]
        net = connected_name(pads[key].GetNetname())
        require(row["Net"] == (net or "NC") and row["Hole (top view)"] == hole(pads[key]),
                f"Stale harness pin: {key}")
        if net:
            destination = row["Schematic destination reference"], row["Destination contact"]
            require(source[destination] == net, f"Harness destination mismatch: {key}")

    # Compact controller harness: one contact per signal/supply, with selected
    # shared ground returns. In particular, the even columns are not GND buses.
    controller_harness = [r for r in harness
                          if r["Schematic destination reference"] == "A1"]
    expected_headers = {
        "J101": ["M1_STEP", "GND", "M1_DIR", "M1_ALM", "M2_STEP", "GND",
                 "M2_DIR", "+3V3", "M3_STEP", "GND", "M3_DIR", "STEPPERS_EN_N"],
        "J102": ["M1_ENC_A", "GND", "M1_ENC_B", "GND", "M1_ENC_Z", "GND"],
        "J108": ["M1_REF", "M2_REF", "M3_REF", "GND"],
        "J113": ["TMC_UART_TX", "GND", "TMC_UART_RX", "M2_INDEX",
                 "M2_DIAG", "M3_INDEX", "M3_DIAG", "GND"],
    }
    require({r["Perfboard header"] for r in controller_harness} == expected_headers.keys(),
            "Unexpected or missing Nucleo harness header")
    for ref, nets in expected_headers.items():
        require({pin: p.GetNetname() for (r, pin), p in pads.items() if r == ref}
                == {str(i): net for i, net in enumerate(nets, 1)},
                f"Compact controller header pinout differs: {ref}")
    counts = collections.Counter(r["Net"] for r in controller_harness)
    expected_signals = {net for net in controller_routes
                        if net.startswith(("M1_", "M2_", "M3_", "STEPPERS_", "TMC_"))}
    require(counts.pop("GND", 0) == 9 and set(counts) == expected_signals | {"+3V3"}
            and all(count == 1 for count in counts.values()),
            "Controller harness needs each signal/supply once and nine deliberate ground returns")

    report = [f"KiCad {pcb.GetBuildVersion()}: native project verification",
              "PASS: one continuous 39 x 48 hole grid (A1:AV39), 2.54 mm pitch, 1872 holes.",
              "PASS: nominal outline 99.06 x 121.92 mm; outer hole-centre span 96.52 x 119.38 mm.",
              f"PASS: {len(footprints)} footprints linked to schematic symbols; values, library IDs and pins match.",
              f"PASS: {len(pads)} unique component holes and {len(vias)} wire passages on the 2.54 mm grid.",
              f"PASS: {len(wires)} scheduled connections span all {len(netpads)} connected nets.",
              f"PASS: {sum(w['sides'] == 'bottom wire' for w in wires)} underside-only wires; "
              f"{sum(w['sides'] != 'bottom wire' for w in wires)} single crossovers with dedicated free holes.",
              "PASS: at most three scheduled wire ends per component solder joint.",
              f"PASS: {len(harness)} header positions and their external destinations match the schematic.",
              "PASS: four Nucleo harnesses; 21 distinct signal/supply contacts and nine ground returns.",
              "PASS: STEP and encoder pairs retain ground returns; reference/UART/DIAG share returns; ALM uses J101.4.",
              f"PASS: {len(controller_routes)} motor, encoder, reference and storage contacts agree with CubeMX.",
              "PASS: CubeMX reference/ALM/DIAG pulls and edges, encoder index edge and disabled startup polarity match wiring.",
              "PASS: DM542T ALM uses PF2/CN9.17, 4.7k pull-up to 3.3 V and GND return; high = fault/open.",
              "PASS: shared USART2 bus, 1k TX resistor, RX FIFO and driver address straps 0/1 match firmware.",
              "PASS: all three module bodies fit; direct mounting contacts match system wiring.",
              "PASS: buck VIN feeds J112 and separate TMC power branches; all 11 power links are underneath.",
              "PASS: TMC logic grounds join local power returns; motor current has dedicated return wiring.",
              "PASS: 24 V appears only on designated power pads; C1/C2 polarity and module power contacts match.",
              "PASS: all parts on top; R10-R12 are DNP; 24 V and logic rails remain distinct.",
              "Native ERC and DRC (including schematic parity): see ERC.rpt and DRC.rpt.",
              "Wire endpoints/passages are checked; routing lengths and sides need review after route edits.",
              "This checks the wiring model, not an assembled circuit or transistor switching speed.",
              f"Board SHA256: {hashlib.sha256(BOARD.read_bytes()).hexdigest()}"]
    for path in [SCHEMATIC, *sorted((ROOT / 'sheets').glob('*.kicad_sch'))]:
        report.append(f"{path.relative_to(ROOT)} SHA256: {hashlib.sha256(path.read_bytes()).hexdigest()}")
    (EXPORTS / "Verification.txt").write_text("\n".join(report) + "\n")
    return components, footprints, pads


def tables(components, footprints, pads):
    write_csv("Holes.csv", ["Reference", "Value", "Pin", "Hole (top view)", "Net"],
              [[ref, footprints[ref].GetValue(), pin, hole(pads[ref, pin]),
                connected_name(pads[ref, pin].GetNetname()) or "NC"]
               for ref, pin in sorted(pads, key=lambda k: (natural(k[0]), natural(k[1])))])
    write_csv("System_Parts.csv", ["Reference", "Part or value", "Quantity", "Do not populate", "Location"],
              [[ref, components[ref].findtext("value"), 1,
                "DNP" if components[ref].find("property[@name='dnp']") is not None else "",
                ("On U1 (existing barrel jack)" if ref == "J1" else
                 f"Perfboard module on {MOUNTED_MODULES[ref]}" if ref in MOUNTED_MODULES else
                 "Perfboard" if ref in footprints else "External")]
               for ref in sorted(components, key=natural)])


def assembly_pdf(tmp):
    config = tmp / "config"
    colors = config / "10.0/colors"
    colors.mkdir(parents=True)
    shutil.copyfile(ROOT / "tools/perfboard-print.json", colors / "perfboard-print.json")
    env = dict(os.environ, KICAD_CONFIG_HOME=str(config))
    views = [
        ("COMPONENT PLACEMENT - TOP VIEW", "F.Fab,F.SilkS,Dwgs.User,Cmts.User,Edge.Cuts", ["--sketch-pads-on-fab-layers"]),
        ("TOP JUMPERS - COMPONENT SIDE", "F.Cu,F.SilkS,Dwgs.User,Cmts.User,Edge.Cuts", []),
        ("BOTTOM WIRES - SOLDER SIDE, ALREADY MIRRORED", "B.Cu,B.SilkS,Dwgs.User,Edge.Cuts", ["--mirror"]),
        ("BOTH WIRE LAYERS - TOP VIEW", "F.Cu,B.Cu,F.SilkS,Dwgs.User,Cmts.User,Edge.Cuts", []),
    ]
    pdfs = []
    for page, (title, layers, flags) in enumerate(views, 1):
        board = pcb.LoadBoard(str(BOARD))
        for item in board.GetDrawings():
            if isinstance(item, pcb.PCB_TEXT) and item.GetText().startswith("PERFBOARD ASSEMBLY MAP"):
                item.SetText(f"{page} / 4  {title}")
        path = tmp / f"view-{page}.kicad_pcb"
        pcb.SaveBoard(str(path), board)
        pdfs.append(path.with_suffix(".pdf"))
        run("kicad-cli", "pcb", "export", "pdf", "--theme", "perfboard-print",
            "--mode-single", "--layers", layers, "-o", pdfs[-1], *flags, path, env=env)
    run("pdfunite", *pdfs, EXPORTS / "Assembly.pdf")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true", help="Refresh checks only; skip PDF and table exports")
    args = parser.parse_args()
    EXPORTS.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="protomate-kicad-") as directory:
        tmp = Path(directory)
        run("kicad-cli", "sch", "erc", "--severity-all", "--exit-code-violations",
            "-o", EXPORTS / "ERC.rpt", SCHEMATIC)
        run("kicad-cli", "pcb", "drc", "--schematic-parity", "--severity-all",
            "--exit-code-violations", "-o", EXPORTS / "DRC.rpt", BOARD)
        netlist = tmp / "source.xml"
        run("kicad-cli", "sch", "export", "netlist", "--format", "kicadxml", "-o", netlist, SCHEMATIC)
        data = verify(netlist, pcb.LoadBoard(str(BOARD)))
        if not args.check_only:
            tables(*data)
            run("kicad-cli", "sch", "export", "pdf", "-o", EXPORTS / "Wiring.pdf", SCHEMATIC)
            assembly_pdf(tmp)
    print("Project checks passed." if args.check_only else "Project checks and exports complete.")


if __name__ == "__main__":
    main()
