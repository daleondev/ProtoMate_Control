#!/usr/bin/env python3
"""Reconcile the purchasing list with the released, fitted component references."""
import csv
import hashlib
import json
from collections import Counter
from design import ROOT, BOARDS


def main():
    folder = ROOT / 'procurement'
    sourcing = json.loads((folder / 'sourcing.json').read_text())
    rows = list(csv.DictReader((folder / 'components.csv').open()))
    requested = Counter()
    hashes = {}
    for name, config in BOARDS.items():
        parts = json.loads((ROOT / name / 'components.json').read_text())
        fitted = {p['ref']: p for p in parts if p['assembly'] != 'PCB'}
        seen = set()
        for row in [r for r in rows if r['Board'] == name]:
            assert row['Revision'] == config['revision']
            refs = {s.strip() for s in row['Designators'].split(',')}
            assert not refs & seen
            assert int(row['Installed quantity']) == len(refs)
            for ref in refs:
                assert fitted[ref]['mpn'] == row['Original BOM MPN']
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
    (folder / 'sourcing.json').write_text(json.dumps(sourcing, indent=2) + '\n')
    report = dict(result='PASS', fitted_components=sum(int(r['Installed quantity']) for r in rows),
                  purchased_pieces=sum(purchased.values()), distinct_order_lines=len(purchased),
                  source_bom_sha256=hashes, stock='Indexed evidence only; confirm live basket availability.')
    (ROOT / 'manufacturing/verification/procurement-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report)


if __name__ == '__main__':
    main()
