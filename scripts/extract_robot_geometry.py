#!/usr/bin/env python3
"""Offline nominal CAD inspection. Requires FreeCAD's Python module, not a runtime dependency.

Run from any directory; outputs deterministic JSON beside the source STEP.
Selectors identify this particular export and fail if its naming/structure changes.
No collision STL is opened, and no source CAD file is modified.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}
LINKS = ('base_link', 'link_1', 'link_2', 'link_3')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rounded(value):
    if isinstance(value, float):
        return round(value, 6) + 0.0
    if isinstance(value, (list, tuple)):
        return [rounded(x) for x in value]
    if isinstance(value, dict):
        return {k: rounded(v) for k, v in value.items()}
    return value


def center(bounds):
    return [(bounds[i] + bounds[i + 3]) / 2 for i in range(3)]


def subtract(a, b):
    return [x - y for x, y in zip(a, b)]


def dae_inventory(directory):
    result = {}
    for name in LINKS:
        path = directory / name / f'{name}.dae'
        root = ET.parse(path).getroot()
        if root.find('c:asset/c:unit', NS).get('meter') != '1' or root.findtext('c:asset/c:up_axis', namespaces=NS) != 'Z_UP':
            raise ValueError(f'{path}: expected metre/Z_UP Fusion export')
        geometry = {g.get('id'): g for g in root.findall('c:library_geometries/c:geometry', NS)}
        bodies = []
        for node in root.findall('c:library_visual_scenes/c:visual_scene/c:node', NS):
            if any(node.find('c:' + t, NS) is not None for t in ('matrix', 'translate', 'rotate', 'scale', 'node')):
                raise ValueError('Expected flat, baked Fusion link geometry')
            mesh = geometry[node.find('c:instance_geometry', NS).get('url')[1:]].find('c:mesh', NS)
            source = mesh.find('c:vertices/c:input[@semantic="POSITION"]', NS).get('source')[1:]
            values = list(map(float, mesh.find(f'c:source[@id="{source}"]/c:float_array', NS).text.split()))
            if not values or len(values) % 3 or not all(math.isfinite(v) for v in values):
                raise ValueError('Invalid mesh coordinates')
            bounds = [min(values[i::3]) * 1000 for i in range(3)] + [max(values[i::3]) * 1000 for i in range(3)]
            bodies.append({'name': node.get('name'), 'bounds_mm': bounds})
        result[name] = {'path': str(path.relative_to(ROOT)), 'sha256': sha(path), 'bodies': bodies,
                        'bounds_mm': [min(b['bounds_mm'][i] for b in bodies) for i in range(3)] +
                                     [max(b['bounds_mm'][i+3] for b in bodies) for i in range(3)]}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freecad-lib', type=Path, default=Path('/usr/lib/freecad/lib'))
    parser.add_argument('--check', action='store_true', help='compare with committed measurements; write nothing')
    args = parser.parse_args()
    sys.path.insert(0, str(args.freecad_lib))
    import FreeCAD as App
    import Import
    import Part

    source = ROOT / 'docs/robot/SCARA_Robot.step'
    doc = App.newDocument('RobotMeasurements')
    print('Importing STEP with FreeCAD/OpenCascade...', flush=True)
    Import.insert(str(source), doc.Name)
    doc.recompute()
    features = {}
    for obj in doc.Objects:
        if obj.TypeId != 'Part::Feature' or obj.Shape.isNull():
            continue
        shape = obj.Shape.copy()
        shape.Placement = obj.getGlobalPlacement().multiply(obj.Placement.inverse()).multiply(shape.Placement)
        box = shape.optimalBoundingBox(False, False)
        bounds = [box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax]
        cylinders = []
        for index, face in enumerate(shape.Faces, 1):
            surface = face.Surface
            if isinstance(surface, Part.Cylinder):
                cylinders.append({'face': index, 'radius_mm': surface.Radius,
                                  'axis': list(surface.Axis), 'axis_point_mm': list(surface.Center)})
        features[obj.Name] = {'label': obj.Label, 'parents': [p.Name for p in obj.InList],
                              'bounds_mm': bounds, 'size_mm': subtract(bounds[3:], bounds[:3]),
                              'cylinders': cylinders}
    print(f'Measured {len(features)} STEP bodies; checking link registration...', flush=True)
    links = dae_inventory(ROOT / 'src/simulation/assets/robot')

    def feature(index, label):
        key = f'Part__Feature{index:03}'
        item = features[key]
        if label not in item['label']:
            raise ValueError(f'Changed STEP selector {key}: expected {label}')
        return item

    def mesh(link, *terms):
        found = [b for b in links[link]['bodies'] if all(t in b['name'] for t in terms)]
        if len(found) != 1:
            raise ValueError(f'Expected one {link} mesh matching {terms}, found {len(found)}')
        return found[0]

    def register(link, index, label, *terms):
        body = feature(index, label)
        visual = mesh(link, *terms)
        delta = subtract(center(body['bounds_mm']), center(visual['bounds_mm']))
        links[link]['rest_translation_mm'] = delta
        links[link]['registration_anchor'] = {'step': f'Part__Feature{index:03}', 'mesh': visual['name']}
        return delta

    origins = [register('base_link', 127, 'J1-HOME', 'J1-HOME', '/ Switch plunger'),
               register('link_1', 170, 'Link1 beam', 'Link1 beam'),
               register('link_2', 276, 'Link2 beam', 'Link2 beam'),
               register('link_3', 285, 'Z21 MGN12H', 'guide block -1 1')]
    # Independently check a second feature on every link (not the fitted anchor).
    checks = [('base_link', 132, 'J1-HOME', ('J1-HOME', '/ PCB and connector')),
              ('link_1', 27, 'B07 Aluminium', ('B07 Aluminium',)),
              ('link_2', 364, 'Z-HOME', ('Z-HOME', '/ Switch plunger')),
              ('link_3', 288, 'Z21 MGN12H', ('guide block 1 2',))]
    for link, index, label, terms in checks:
        actual = feature(index, label)['bounds_mm']
        local = mesh(link, *terms)['bounds_mm']
        offset = links[link]['rest_translation_mm']
        residual = max(abs(actual[i] - local[i] - offset[i % 3]) for i in range(6))
        if residual > .002:
            raise ValueError(f'{link}: STEP/DAE registration residual {residual} mm')
        links[link]['cross_check'] = {'step': f'Part__Feature{index:03}', 'max_bound_residual_mm': residual}

    def cylinder(index, label, radius):
        return next(c for c in feature(index, label)['cylinders'] if abs(c['radius_mm'] - radius) < 1e-5)

    j1 = cylinder(27, 'B07 Aluminium', 12.5)['axis_point_mm']
    j2 = cylinder(188, 'J2-07 Aluminium', 12.5)['axis_point_mm']
    cutter = cylinder(284, 'Z90 Dremel', 1.6)['axis_point_mm']
    stage = doc.getObject('SCARA_J3_Rev_B___Dual_Rail_2040').getGlobalPlacement().Base
    lengths = [math.hypot(j2[0] - j1[0], j2[1] - j1[1]), stage.x - j2[0], cutter[0] - stage.x]
    tip_z = feature(284, 'Z90 Dremel')['bounds_mm'][2]
    sensors = []
    for name, parent, plunger, pcb, outward in [('J1', 0, 127, 132, [1,0,0]), ('J2', 1, 256, 261, [1,0,0]), ('Z', 2, 364, 369, [0,-1,0])]:
        world = center(feature(plunger, name + '-HOME')['bounds_mm'])
        holes = [c for c in feature(pcb, name + '-HOME')['cylinders'] if abs(c['radius_mm']-1.5) < 1e-5]
        root = cylinder(plunger + 1, name + '-HOME', .49961168231284)
        # Any point along the cylinder axis describes the same lever hinge.
        # Choose the free plunger's transverse center for readable local coordinates.
        root_point = list(root['axis_point_mm'])
        for axis, direction in enumerate(root['axis']):
            if abs(direction) > .999:
                root_point[axis] = world[axis]
        sensors.append({'name': name, 'parent': LINKS[parent], 'step_plunger': f'Part__Feature{plunger:03}',
                        'step_lever': f'Part__Feature{plunger + 1:03}',
                        'local_lever_root_mm': subtract(root_point, origins[parent]),
                        'lever_press_axis': root['axis'],
                        'world_plunger_center_mm': world, 'local_plunger_center_mm': subtract(world, origins[parent]),
                        'outward_axis': outward, 'pcb_mounting_holes': holes,
                        'trigger_position': None, 'hysteresis': None})

    # Preserve the exact product descriptions that carry nominal drive data and stale design notes.
    products = []
    for match in re.finditer(r'#(\d+)=PRODUCT\((.*?)\);', source.read_text(), re.S):
        fields = re.findall(r"'((?:[^']|'')*)'", match[2])
        if len(fields) >= 3 and any(s in fields[1] for s in ('100T', '20T', 'M8x350', 'Link 1 -', 'Link 2 -', 'Compact base', 'SCARA J2 Rev B Compact', 'SCARA J3 Rev B', 'HOME Limit')):
            products.append({'entity': '#' + match[1], 'name': fields[1], 'description': fields[2].replace("''", "'").replace('\n', '')})
    ratios = []
    for driven, motor in [(28,31), (189,192), (305,314)]:
        teeth = [int(re.search(r'(\d+)T', features[f'Part__Feature{i:03}']['label'])[1]) for i in (driven,motor)]
        ratios.append(teeth[0]/teeth[1])
    if not any('1.25' in p['description'] and 'assumed' in p['description'] for p in products if 'M8x350' in p['name']):
        raise ValueError('Missing documented nominal screw pitch assumption')
    report = {'schema': 1, 'units': 'mm', 'source': {'path': str(source.relative_to(ROOT)), 'sha256': sha(source)},
              'method': 'FreeCAD/OpenCascade evaluated global assembly placements, analytic cylinder axes, optimal bounds without triangulation or shape tolerance; DAE float vertices for registration only.',
              'precision': 'Six decimal places are numerical reporting precision, NOT physical accuracy. STEP tool and purchased-part envelopes may be approximate.',
              'kinematics': {'link_lengths_mm': lengths, 'j1_axis_point_mm': j1, 'j2_axis_point_mm': j2,
                             'z_stage_datum_mm': list(stage), 'rest_tcp_mm': [cutter[0],cutter[1],tip_z],
                             'tool_tip_local_mm': [cutter[0]-origins[3][0],cutter[1]-origins[3][1],tip_z-origins[3][2]]},
              'nominal_drive': {'motor_reduction': ratios, 'screw_lead_mm': 1.25, 'screw_lead_status': 'CAD author assumption; smooth M8 thread envelope; not geometrically measured'},
              'sensors': sensors, 'visual_links': links, 'step_bodies': features, 'product_metadata': products,
              'unresolved': ['Electrical switch activation/hysteresis and physical home coordinates are absent.',
                             'Joint signs, operational travel limits, collision clearance, and calibration are not established by static STEP.',
                             'J2 assembly description says 4:1; selected 100T/20T parts say 5:1.',
                             'J1 description mentions 3MGT; selected pulley labels/geometry say 2GT.',
                             'Old link/base/shaft body descriptions disagree with evaluated geometry; see geometry.md.']}
    # Keep complete bounds, but analytic details only for useful shaft/pulley/sensor references.
    analytic = {27,28,31,128,132,188,189,192,257,261,284,302,305,314,365,369}
    for key, body in features.items():
        index = int(key.removeprefix('Part__Feature') or 0)
        if index not in analytic:
            body.pop('cylinders')
    output = ROOT / 'docs/robot/geometry.json'
    text = json.dumps(rounded(report), indent=2, ensure_ascii=False) + '\n'
    if args.check:
        if output.read_text() != text:
            raise SystemExit('CAD measurements differ; regenerate and review.')
        print('PASS: STEP geometry and all four visual registrations match the saved report.')
    else:
        output.write_text(text)
        print(f'Wrote {output}; link lengths {rounded(lengths)} mm')
    App.closeDocument(doc.Name)


if __name__ == '__main__':
    main()
