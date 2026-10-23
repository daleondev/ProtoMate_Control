#!/usr/bin/env python3
"""Reconcile purchasing with the current KiCad components and generator data."""
import csv
import hashlib
import json
from collections import Counter
from design import ROOT, BOARDS, PARTS
from sexpr import parse, children, property_value


def main():
    folder = ROOT / 'procurement'
    sourcing = json.loads((folder / 'sourcing.json').read_text())
    selections = {p['original_mpn']: p for p in sourcing['parts']}
    rows = list(csv.DictReader((folder / 'components.csv').open()))
    requested = Counter()
    hashes = {}
    current_hashes = {}
    for name, config in BOARDS.items():
        parts = json.loads((ROOT / name / 'components.json').read_text())
        fitted = {p['ref']: p for p in parts if p['assembly'] != 'PCB'}
        pcb = ROOT / name / (name + '.kicad_pcb')
        footprints = {property_value(n, 'Reference'): n
                      for n in children(parse(pcb.read_text()), 'footprint')}
        symbols = {}
        sources = [pcb, ROOT / name / 'components.json', ROOT / name / 'Adapter.kicad_sym']
        for path in sorted((ROOT / name).glob('*.kicad_sch')):
            if path.name.startswith('_restore'):
                continue
            sources.append(path)
            for node in children(parse(path.read_text()), 'symbol'):
                reference = property_value(node, 'Reference')
                assert reference not in symbols, (name, reference, 'duplicate schematic reference')
                symbols[reference] = node
        assert set(fitted) <= footprints.keys() and set(fitted) <= symbols.keys()
        for ref, part in fitted.items():
            for field, key in [('Value', 'value'), ('Manufacturer', 'mfr'), ('MPN', 'mpn')]:
                assert property_value(footprints[ref], field) == part[key], (name, ref, 'PCB', field)
                assert property_value(symbols[ref], field) == part[key], (name, ref, 'schematic', field)
            if part['kind'] in PARTS:
                assert tuple(part[k] for k in ('value', 'mfr', 'mpn', 'footprint')) == PARTS[part['kind']]
        for path in sources:
            current_hashes[path.relative_to(ROOT).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
        seen = set()
        for row in [r for r in rows if r['Board'] == name]:
            assert row['Revision'] == config['revision']
            selection = selections[row['Original BOM MPN']]
            assert row['Purchase MPN'] == selection['purchase_mpn']
            assert row['Mouser part number'] == selection['mouser_sku']
            refs = {s.strip() for s in row['Designators'].split(',')}
            assert not refs & seen
            assert int(row['Installed quantity']) == len(refs)
            for ref in refs:
                assert fitted[ref]['mpn'] == row['Purchase MPN'], (name, ref, 'purchase MPN mismatch')
                assert fitted[ref]['mfr'].casefold() == row['Purchase manufacturer'].casefold()
                assert fitted[ref]['value'] == row['Circuit value']
                assert fitted[ref]['footprint'] == row['PCB package']
            seen |= refs
            if int(row['Purchase quantity']):
                requested[row['Mouser part number']] += int(row['Purchase quantity'])
        assert seen == set(fitted), (name, seen ^ set(fitted))
        separate = list(csv.DictReader((folder / (name + '-components.csv')).open()))
        assert separate == [r for r in rows if r['Board'] == name]
        bom = ROOT / 'manufacturing' / f"{name}-rev-{config['revision']}" / 'bom-all.csv'
        hashes[bom.relative_to(ROOT).as_posix()] = hashlib.sha256(bom.read_bytes()).hexdigest()
    for item in sourcing['accessories']:
        requested[item['mouser_sku']] += item['quantity']
    order = list(csv.reader((folder / 'mouser-order.csv').open()))
    purchased = Counter({sku: int(quantity) for sku, quantity in order[1:]})
    assert len(order) - 1 == len(purchased), 'Duplicate order lines'
    assert requested == purchased, (requested - purchased, purchased - requested)
    assert purchased['538-505110-6091'] == 2 and purchased['538-15018-0563'] == 1
    sourcing['source_bom_sha256'] = hashes
    current_hashes['tools/design.py'] = hashlib.sha256((ROOT / 'tools/design.py').read_bytes()).hexdigest()
    sourcing['current_kicad_sha256'] = current_hashes
    (folder / 'sourcing.json').write_text(json.dumps(sourcing, indent=2) + '\n')
    report = dict(result='PASS', fitted_components=sum(int(r['Installed quantity']) for r in rows),
                  purchased_pieces=sum(purchased.values()), distinct_order_lines=len(purchased),
                  source_bom_sha256=hashes, stock='Indexed evidence only; confirm live basket availability.')
    report['current_kicad_sha256'] = current_hashes
    (ROOT / 'manufacturing/verification/procurement-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"PASS: {report['fitted_components']} fitted components agree across KiCad, "
          f"generator data and procurement; {len(purchased)} order lines, "
          f"{sum(purchased.values())} purchased pieces.")


if __name__ == '__main__':
    main()
