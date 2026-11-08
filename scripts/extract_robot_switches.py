#!/usr/bin/env python3
"""Extract named switch levers and rotary cam bodies without FreeCAD.

All inputs remain unchanged. --check verifies generated assets and source hashes.
The simplified whole-link collision STLs bridge voids between rotary cams.
Use the exact lever and cam-body surfaces, retaining their original local frames.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'src/simulation/assets/robot'
NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def extract(path, body="/ Spring lever - supplier free position"):
    source = path.read_bytes()
    root = ET.fromstring(source)
    if root.find('c:asset/c:unit', NS).get('meter') != '1' or root.findtext('c:asset/c:up_axis', namespaces=NS) != 'Z_UP':
        raise ValueError(f'{path}: expected metre/Z_UP Fusion export')
    nodes = [n for n in root.findall('c:library_visual_scenes/c:visual_scene/c:node', NS)
             if body in n.get('name', '')]
    if len(nodes) != 1:
        raise ValueError(f'{path}: expected exactly one named contact body')
    node = nodes[0]
    if any(node.find('c:' + t, NS) is not None for t in ('matrix', 'translate', 'rotate', 'scale', 'node')):
        raise ValueError('Expected baked parent-link coordinates')
    geometry = node.find('c:instance_geometry', NS).get('url')[1:]
    mesh = root.find(f'c:library_geometries/c:geometry[@id="{geometry}"]/c:mesh', NS)
    position = mesh.find('c:vertices/c:input[@semantic="POSITION"]', NS).get('source')[1:]
    points = list(map(float, mesh.find(f'c:source[@id="{position}"]/c:float_array', NS).text.split()))
    if not points or len(points) % 3 or not all(math.isfinite(p) for p in points):
        raise ValueError('Invalid lever vertices')
    triangles = []
    for group in mesh.findall('c:triangles', NS):
        stride = 1 + max(int(i.get('offset')) for i in group.findall('c:input', NS))
        offset = int(group.find('c:input[@semantic="VERTEX"]', NS).get('offset'))
        indices = list(map(int, group.find('c:p', NS).text.split()))
        if len(indices) != int(group.get('count')) * 3 * stride:
            raise ValueError('Invalid lever triangle count')
        vertices = indices[offset::stride]
        if any(v < 0 or v >= len(points) // 3 for v in vertices):
            raise ValueError('Invalid lever vertex index')
        for i in range(0, len(vertices), 3):
            triangles.append([points[3*v + j] for v in vertices[i:i+3] for j in range(3)])
    if not triangles:
        raise ValueError('No lever triangles')
    data = b'Fusion contact body, metres/Z-up, original link frame'.ljust(80, b'\0') + struct.pack('<I', len(triangles))
    for triangle in triangles:
        data += struct.pack('<12fH', 0, 0, 0, *triangle, 0)
    return data, {'source': str(path.relative_to(ROOT)), 'sha256': sha(source), 'body': node.get('name'),
                  'triangles': len(triangles), 'output_sha256': sha(data)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = {}
    metadata = {}
    for name, parent, moving in [('j1', 'base_link', 'link_1'), ('j2', 'link_1', 'link_2'), ('z', 'link_2', 'link_3')]:
        data, source = extract(ASSETS / parent / f'{parent}.dae')
        collision = ASSETS / moving / f'{moving}_collision.stl'
        if name != 'z':
            body = ('/ Rigid continuous thrust backing and Link1 saddle' if name == 'j1'
                    else 'J2-03 Rotating lower bearing housing and Link2 sleeve - PRINT:1 / Bearing deck ring')
            cam_data, cam_source = extract(ASSETS / moving / f'{moving}.dae', body)
            collision = ASSETS / 'sensors' / f'{name}_cams.stl'
            outputs[collision] = cam_data
            source['cam_source'] = cam_source
        collision_data = outputs.get(collision) or collision.read_bytes()
        source['moving_collision'] = {'path': str(collision.relative_to(ROOT)), 'sha256': sha(collision_data)}
        outputs[ASSETS / 'sensors' / f'{name}_lever.stl'] = data
        metadata[name] = source
    outputs[ASSETS / 'sensors/sources.json'] = (json.dumps(metadata, indent=2) + '\n').encode()
    for path, data in outputs.items():
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                raise SystemExit(f'Stale switch geometry: {path}; rerun scripts/extract_robot_switches.py and review')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    print('PASS: CAD lever/cam source and contact mesh fingerprints' if args.check else 'Generated CAD lever/cam meshes and source fingerprints')


if __name__ == '__main__':
    main()
