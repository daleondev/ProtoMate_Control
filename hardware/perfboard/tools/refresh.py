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
DEFAULT_IOC = ROOT.parents[1] / "external/CubeMX/CubeMX.ioc"
MODULE_MOUNTS = {"A3": "J105", "A4": "J106"}
MOUNTED_MODULES = {"U1": "J103", **MODULE_MOUNTS}
# The purchased 65 x 47 hole board is mounted with its long edge vertical.
GRID_COLUMNS, GRID_ROWS = 47, 65
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
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(headings)
        writer.writerows(rows)


def natural(text):
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", text)]


def row_name(row):
    letters = ""
    while row:
        row, remainder = divmod(row - 1, 26)
        letters = chr(65 + remainder) + letters
    return letters


def hole(item):
    pos = item.GetPosition()
    x, y = ((pcb.ToMM(n) - GRID_ORIGIN) / GRID_PITCH for n in (pos.x, pos.y))
    require(abs(x - round(x)) < 1e-6 and abs(y - round(y)) < 1e-6,
            f"Off-grid position: {pos}")
    require(0 <= round(x) < GRID_COLUMNS and 0 <= round(y) < GRID_ROWS,
            f"Position outside the {GRID_COLUMNS} x {GRID_ROWS} hole field")
    return row_name(round(y) + 1) + str(round(x) + 1)


def verify(netlist, board, ioc_path):
    edges = [item for item in board.GetDrawings() if item.GetLayer() == pcb.Edge_Cuts]
    corners = {(round(pcb.ToMM(v.x), 6), round(pcb.ToMM(v.y), 6))
               for edge in edges for v in (edge.GetStart(), edge.GetEnd())}
    require(len(edges) == 4 and corners == {
        (BOARD_LEFT, BOARD_TOP), (BOARD_RIGHT, BOARD_TOP),
        (BOARD_RIGHT, BOARD_BOTTOM), (BOARD_LEFT, BOARD_BOTTOM)},
        f"Board outline differs from the {GRID_COLUMNS} x {GRID_ROWS} grid with half-pitch margins")
    grid = [item for item in board.GetDrawings()
            if isinstance(item, pcb.PCB_SHAPE) and item.GetLayer() == pcb.Dwgs_User
            and item.GetShape() == pcb.SHAPE_T_CIRCLE]
    require(len(grid) == GRID_COLUMNS * GRID_ROWS and
            len({hole(item) for item in grid}) == GRID_COLUMNS * GRID_ROWS,
            f"Drawing must show all {GRID_COLUMNS * GRID_ROWS} holes exactly once")
    document = ET.parse(netlist).getroot()
    components = {c.get("ref"): c for c in document.findall("./components/comp")}
    onboard = {ref: c for ref, c in components.items()
               if c.find("property[@name='exclude_from_board']") is None}
    footprints = {f.GetReference(): f for f in board.GetFootprints()}
    require(onboard.keys() == footprints.keys(), "Schematic/board component sets differ")
    source = {(n.get("ref"), n.get("pin")): net.get("name")
              for net in document.findall("./nets/net") for n in net.findall("node")}
    ioc = dict(line.split("=", 1) for line in
               ioc_path.read_text().splitlines()
               if "=" in line and not line.startswith("#"))
    require(ioc.get("Mcu.CPN") == "STM32H747XIH6", "Expected the GIGA's STM32H747XIH6")
    # Arduino silkscreen labels, not ordinal positions on a connector. The
    # final field is the current firmware label; UART pins have no GPIO label.
    controller_routes = {
        "M1_ALM": ("PB1", "A3", "GPXTI1", "M1_ALARM"),
        "M1_STEP": ("PA0", "A7", "S_TIM2_CH1_ETR", "M1_STEP"),
        "M2_STEP": ("PA2", "D3", "S_TIM2_CH3", "M2_STEP"),
        "M3_STEP": ("PA3", "D2", "S_TIM2_CH4", "M3_STEP"),
        "M1_DIR": ("PJ8", "D4", "GPIO_Output", "M1_DIR"),
        "M2_DIR": ("PD13", "D6", "GPIO_Output", "M2_DIR"),
        "M3_DIR": ("PH13", "D16", "GPIO_Output", "M3_DIR"),
        "STEPPERS_EN_N": ("PC7", "D15", "GPIO_Output", "STEPPERS_ENABLE"),
        "M1_ENC_A": (r"PB4\ (NJTRST)", "D7", "S_TIM3_CH1", "M1_ENC_A"),
        "M1_ENC_B": ("PA7", "D5", "S_TIM3_CH2", "M1_ENC_B"),
        "M1_ENC_Z": ("PG7", "D53", "GPXTI7", "M1_ENC_Z"),
        "M1_REF": ("PE4", "D49", "GPXTI4", "M1_REFERENCE"),
        "M2_REF": ("PE5", "D51", "GPXTI5", "M2_REFERENCE"),
        "M3_REF": ("PB2", "D47", "GPXTI2", "M3_REFERENCE"),
        "TMC_UART_TX": ("PD5", "D18", "USART2_TX", None),
        "TMC_UART_RX": ("PD6", "D19", "USART2_RX", None),
        "M2_INDEX": ("PB8", "D8", "GPXTI8", "M2_INDEX"),
        "M3_INDEX": ("PB9", "D9", "GPXTI9", "M3_INDEX"),
        "M2_DIAG": ("PC3", "A4", "GPXTI3", "M2_DIAG"),
        "M3_DIAG": ("PC0", "A6", "GPXTI0", "M3_DIAG"),
    }
    for net, (pin, contact, signal, label) in controller_routes.items():
        require(source.get(("A1", contact)) == net, f"Controller contact differs: {net}")
        require(ioc.get(f"{pin}.Signal") == signal,
                f"CubeMX GPIO/timer differs from wiring: {net}")
        require(ioc.get(f"{pin}.ContextOwner") == "CortexM4",
                f"Motor peripheral must belong to Cortex-M4: {net}")
        if label is not None:
            require(ioc.get(f"{pin}.GPIO_Label") == label, f"CubeMX label differs: {net}")
    require(components["A1"].findtext("value") == "Arduino GIGA R1 WiFi / ABX00063",
            "Controller symbol must identify the GIGA")
    require({pin: net for (ref, pin), net in source.items() if ref == "A1"} ==
            {**{route[1]: net for net, route in controller_routes.items()},
             "3V3": "+3V3", "GND": "GND"},
            "GIGA harness must carry only motor signals, 3V3 and GND; no buck 5 V or storage bus")
    require(not any(net.startswith(("FLASH_", "SD_")) for net in source.values()),
            "External storage wiring does not belong to the GIGA perfboard")
    for pin in ("PE4", "PE5", "PB2"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLUP" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING_FALLING",
                f"NC reference input requires pull-up and both-edge EXTI: {pin}")
    require(ioc.get("PG7.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
            "Encoder index must use rising-edge EXTI after the polarity-preserving interface")
    require(ioc.get("PC7.PinState") == "GPIO_PIN_SET",
            "Shared active-low enable must start high (drivers disabled)")
    require(ioc.get("TIM2.Prescaler") == "23", "TIM2 must retain the GIGA STEP prescaler")
    for pin in ("PA0", "PA2", "PA3"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLDOWN",
                f"STEP idle GPIO pull must be LOW: {pin}")
    for pin in ("PB8", "PB9"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLDOWN" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING_FALLING",
                f"Driver INDEX requires pull-down and both-edge EXTI: {pin}")
    for pin in ("PC0", "PC3"):
        require(ioc.get(f"{pin}.GPIO_PuPd") == "GPIO_PULLDOWN" and
                ioc.get(f"{pin}.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
                f"Driver DIAG requires pull-down and rising-edge EXTI: {pin}")
    require(ioc.get("PB1.GPIO_PuPd") == "GPIO_PULLUP" and
            ioc.get("PB1.GPIO_ModeDefaultEXTI") == "GPIO_MODE_IT_RISING",
            "DM542T ALM requires pull-up and rising-edge EXTI1")
    for ref, pin, net in (("A2", "ALM+", "M1_ALM"), ("A2", "ALM-", "GND"),
                          ("R30", "1", "+3V3"), ("R30", "2", "M1_ALM"),
                          ("J104", "7", "M1_ALM"), ("J104", "8", "GND"),
                          ("J101", "4", "M1_ALM")):
        require(source.get((ref, pin)) == net, f"DM542T alarm connection differs: {ref}.{pin}")
    require(components["R30"].findtext("value") == "4k7", "ALM needs its 4.7k external 3.3 V pull-up")
    require(ioc.get("USART2.BaudRate") == "115200" and
            ioc.get("USART2.FIFOMode") == "FIFOMODE_ENABLE",
            "Shared driver UART must preserve 115200 baud and RX FIFO")
    # Physical reset bias remains effective before firmware configures GPIOs.
    for ref, value, first, second in (("R7", "470R", "+3V3", "STEPPERS_EN_N"),
                                      ("R8", "10k", "M2_STEP", "GND"),
                                      ("R9", "10k", "M3_STEP", "GND")):
        require(components[ref].findtext("value") == value and
                source[ref, "1"] == first and source[ref, "2"] == second,
                f"Reset-state bias differs: {ref}")
    require(source["R1", "1"] == "M1_STEP" and source["R1", "2"] == source["R2", "1"]
            and source["R2", "2"] == "GND" and components["R1"].findtext("value") == "1k"
            and components["R2"].findtext("value") == "10k",
            "M1 STEP must retain its R1/R2 path to GND during reset")
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
    power_positive = {("J103", "4"), ("J104", "9"), ("J105", "11"),
                      ("J106", "11"), ("C1", "1"), ("C2", "1")}
    require({key for key, p in pads.items() if p.GetNetname() == "+24V"} == power_positive,
            "24 V must connect only to the buck VIN, driver power pins and bulk capacitors")
    dm_terminal = ["+5V", "DM_PUL_N", "+5V", "DM_DIR_N", "+5V", "DM_ENA_N",
                   "M1_ALM", "GND", "+24V", "GND"]
    require({pin: p.GetNetname() for (ref, pin), p in pads.items() if ref == "J104"}
            == {str(i): net for i, net in enumerate(dm_terminal, 1)},
            "DM542T terminal needs control 1-6, alarm 7/8 and 24 V/GND 9/10")
    require(footprints["J104"].GetFPIDAsString() ==
            "ProtoMate_Perfboard:TerminalBlock_1x10_P5.08mm_DM542T"
            and footprints["J104"].GetOrientationDegrees() == 0,
            "DM542T needs a ten-way 5.08 mm screw terminal facing the top edge")
    require("J112" not in components and "J114" not in components,
            "DM542T power and alarm connections must be combined in J104")

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
    # the signal-ground tree. J104.9/10 are the power star points.
    power_links = set()
    for buck_pin, terminal_pin, module_pin in (("4", "1", "11"), ("3", "2", "12")):
        star_pin = str(int(terminal_pin) + 8)
        power_links.add(frozenset((("J103", buck_pin), ("J104", star_pin))))
        for cap, module in (("C1", "J105"), ("C2", "J106")):
            power_links.add(frozenset((("J104", star_pin), (cap, terminal_pin))))
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
                (wire["sides"] == "bottom wire + top jumper" and len(passages) in (2, 4)),
                f"Wire must stay underneath or list each crossover's pair of holes: {wire['wire']}")
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
            "Unexpected or missing GIGA harness header")
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
              f"PASS: one continuous {GRID_COLUMNS} x {GRID_ROWS} hole grid "
              f"(A1:{row_name(GRID_ROWS)}{GRID_COLUMNS}), {GRID_PITCH} mm pitch, "
              f"{GRID_COLUMNS * GRID_ROWS} holes.",
              f"PASS: nominal outline {GRID_COLUMNS * GRID_PITCH:.2f} x {GRID_ROWS * GRID_PITCH:.2f} mm; "
              f"outer hole-centre span {(GRID_COLUMNS - 1) * GRID_PITCH:.2f} x "
              f"{(GRID_ROWS - 1) * GRID_PITCH:.2f} mm.",
              f"PASS: {len(footprints)} footprints linked to schematic symbols; values, library IDs and pins match.",
              f"PASS: {len(pads)} unique component holes and {len(vias)} wire passages on the 2.54 mm grid.",
              f"PASS: {len(wires)} scheduled connections span all {len(netpads)} connected nets.",
              f"PASS: {sum(w['sides'] == 'bottom wire' for w in wires)} underside-only wires; "
              f"{sum(w['sides'] != 'bottom wire' for w in wires)} wires use "
              f"{len(vias) // 2} crossovers with dedicated free holes.",
              "PASS: at most three scheduled wire ends per component solder joint.",
              f"PASS: {len(harness)} header positions and their external destinations match the schematic.",
              "PASS: four GIGA harnesses; 21 distinct signal/supply contacts and nine ground returns.",
              "PASS: STEP and encoder pairs retain ground returns; reference/UART/DIAG share returns; ALM uses J101.4.",
              f"PASS: {len(controller_routes)} motor, encoder, reference and driver contacts match the GIGA schematic.",
              f"PASS: CubeMX pins, M4 ownership, labels, input pulls/edges, UART and disabled startup match wiring: {ioc_path}",
              "PASS: GIGA has only signal/3V3/GND connections; no buck 5 V feed or external storage bus.",
              "PASS: existing R7 enable pull-up and R1/R2, R8/R9 STEP pull-down paths remain fitted.",
              "PASS: DM542T ALM uses A3/PB1, 4.7k pull-up to 3.3 V and GND return; high = fault/open.",
              "PASS: shared UART bus, 1k TX resistor and driver address straps 0/1 match system wiring.",
              "PASS: all three module bodies fit; direct mounting contacts match system wiring.",
              "PASS: J104 combines DM542T control 1-6, ALM 7/8 and 24 V/GND 9/10 in one outward-facing screw terminal.",
              "PASS: buck VIN feeds J104.9/10 and separate TMC power branches; all 11 power links are underneath.",
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
    # Keep page coordinates at 1:1 for both faces. Without a drawing sheet,
    # KiCad 10 offsets mirrored plots and can clip headings on portrait pages.
    # An empty sheet preserves the origin without adding a frame or title block.
    sheet = tmp / "assembly.kicad_wks"
    sheet.write_text('(kicad_wks (version 20231118) (generator "pl_editor"))\n')
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
            "--mode-single", "--scale", "1", "--include-border-title", "--drawing-sheet", sheet,
            "--layers", layers, "-o", pdfs[-1], *flags, path, env=env)
    run("pdfunite", *pdfs, EXPORTS / "Assembly.pdf")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true", help="Refresh checks only; skip PDF and table exports")
    parser.add_argument("--ioc", type=Path, default=DEFAULT_IOC,
                        help="GIGA CubeMX configuration to verify (default: repository external/CubeMX/CubeMX.ioc)")
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
        data = verify(netlist, pcb.LoadBoard(str(BOARD)), args.ioc)
        if not args.check_only:
            tables(*data)
            run("kicad-cli", "sch", "export", "pdf", "-o", EXPORTS / "Wiring.pdf", SCHEMATIC)
            assembly_pdf(tmp)
    print("Project checks passed." if args.check_only else "Project checks and exports complete.")


if __name__ == "__main__":
    main()
