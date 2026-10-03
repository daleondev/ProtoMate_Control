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
    x, y = ((pcb.ToMM(n) - 50.8) / 2.54 for n in (pos.x, pos.y))
    require(abs(x - round(x)) < 1e-6 and abs(y - round(y)) < 1e-6,
            f"Off-grid position: {pos}")
    require(0 <= round(x) < 60 and 0 <= round(y) < 36, "Position outside A1:AJ60")
    row, letters = round(y) + 1, ""
    while row:
        row, remainder = divmod(row - 1, 26)
        letters = chr(65 + remainder) + letters
    return letters + str(round(x) + 1)


def verify(netlist, board):
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
    motor_routes = {
        "M1_STEP": ("PA0", "CN10.29", "S_TIM2_CH1_ETR"),
        "M2_STEP": ("PB10", "CN10.32", "S_TIM2_CH3"),
        "M3_STEP": ("PB11", "CN10.34", "S_TIM2_CH4"),
        "M1_DIR": ("PE12", "CN10.26", "GPIO_Output"),
        "M2_DIR": ("PE13", "CN10.10", "GPIO_Output"),
        "M3_DIR": ("PE14", "CN10.8", "GPIO_Output"),
        "STEPPERS_EN_N": ("PE15", "CN10.30", "GPIO_Output"),
    }
    for net, (pin, contact, signal) in motor_routes.items():
        require(source.get(("A1", contact)) == net, f"Controller contact differs: {net}")
        require(ioc.get(f"{pin}.GPIO_Label") == net and ioc.get(f"{pin}.Signal") == signal,
                f"CubeMX GPIO/timer differs from wiring: {net}")
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
    require(all(p.GetNetname() != "+24V" for p in pads.values()), "24 V on perfboard")

    # Wires.csv is an assembly schedule, maintained alongside routing changes.
    wires = read_csv("Wires.csv")
    graph = collections.defaultdict(set)
    vias = {hole(t): t.GetNetname() for t in board.GetTracks() if isinstance(t, pcb.PCB_VIA)}
    occupied = {hole(p): p.GetNetname() for p in pads.values()}
    require(not (vias.keys() & occupied.keys()), "Wire passage overlaps a component lead")
    for wire in wires:
        a = wire["from_ref"], wire["from_pin"]
        b = wire["to_ref"], wire["to_pin"]
        require(pads[a].GetNetname() == wire["net"] == pads[b].GetNetname(),
                f"Wrong wire net: {wire['wire']}")
        require(hole(pads[a]) == wire["from_hole"] and hole(pads[b]) == wire["to_hole"],
                f"Stale wire endpoint: {wire['wire']}")
        graph[a].add(b)
        graph[b].add(a)
        for passage in filter(None, wire["side_change_holes"].split(", ")):
            require(vias.get(passage, occupied.get(passage)) == wire["net"],
                    f"Wrong wire passage: {wire['wire']}")
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

    report = [f"KiCad {pcb.GetBuildVersion()}: native project verification",
              f"PASS: {len(footprints)} footprints linked to schematic symbols; values, library IDs and pins match.",
              f"PASS: {len(pads)} unique component holes and {len(vias)} wire passages on the 2.54 mm grid.",
              f"PASS: {len(wires)} scheduled connections span all {len(netpads)} connected nets.",
              f"PASS: {len(harness)} header positions and their external destinations match the schematic.",
              "PASS: seven motor controller contacts agree with the CubeMX GPIO/timer assignments.",
              "PASS: all parts on top; R10-R12 are DNP; no 24 V on perfboard.",
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
                "Perfboard" if ref in footprints else "External"]
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
