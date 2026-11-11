#!/usr/bin/env python3
"""Render the reviewed reflection manifest; never infer nodes from C++ text."""
import argparse
import json
from pathlib import Path


def generate(source, destination):
    schema = json.loads(source.read_text())
    lines = ['# ProtoMate OPC UA node reference', '',
             f'Namespace: `{schema["namespace"]}` · model `{schema["version"]}`.', '',
             'Generated from [interface.json](interface.json). NodeIds below are string identifiers in that namespace.', '',
             'Monitoring is read-only. Commands require local permission, a control generation and increasing request IDs.',
             'A receipt acknowledges submission; follow its operation and execution job for acceptance and completion.', '']
    records = {}
    def describe(value):
        if 'array' in value:
            return f'{describe(value["element"])}[{value["array"]}]'
        name = value['type']
        if 'fields' in value:
            if name in records:
                assert records[name] == value['fields'], f'Conflicting record {name}'
            records[name] = value['fields']
            return '`' + name + '`'
        if 'capacity' in value: return f'{name} (≤{value["capacity"]} bytes)'
        if 'values' in value: return name + ' {' + ', '.join(f'{v["name"]}={v["value"]}' for v in value['values']) + '}'
        return name
    for node in schema['nodes']:
        if node['kind'] == 'object':
            lines += ['## ' + node['id'], '', node['description'], '']
        else:
            lines += ['### `' + node['id'] + '`', '', node['description'], '']
            if node['kind'] == 'variable':
                lines += [f'Read: {describe(node["value"])}' + (f'; unit `{node["unit"]}`.' if node['unit'] else '.'), '']
            else:
                lines += [f'Access: **{node["access"]}**.', '', '| Argument | Type |', '|---|---|']
                lines += [f'| `{p["name"]}` | {describe(p["value"])} |' for p in node['inputs']]
                lines += [f'| Return | {describe(node["output"])} |', '']
    emitted = set()
    while len(emitted) != len(records):
        for name, fields in list(records.items()):
            if name in emitted: continue
            emitted.add(name)
            lines += ['## `' + name + '`', '', '| Field | Type | Unit / bounds | Meaning |', '|---|---|---|---|']
            for f in fields:
                bounds = f.get('unit', '')
                if 'minimum' in f: bounds += f' [{f["minimum"]}, {f["maximum"]}]'
                lines += [f'| `{f["name"]}` | {describe(f["value"])} | {bounds} | {f["description"]} |']
            lines += ['']
    destination.write_text('\n'.join(lines))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    generate(args.manifest, args.output)
