#!/usr/bin/env python3
"""Fast source/report/runtime parity checks. Full extraction is an offline FreeCAD tool."""
import hashlib
import json
import math
from pathlib import Path
import re

root = Path(__file__).resolve().parents[3]
report = json.loads((root / 'docs/robot/geometry.json').read_text())
assert report['schema'] == 1
for source in [report['source'], *report['visual_links'].values()]:
    with (root / source['path']).open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    assert actual == source['sha256'], f"Stale CAD measurements for {source['path']}; rerun scripts/extract_robot_geometry.py and review"
assert len(report['step_bodies']) == 372
assert sum(len(x['bodies']) for x in report['visual_links'].values()) == 372
header = (root / 'src/simulation/CadGeometry.hpp').read_text()

def values(name):
    initializer = re.search(r'\b' + name + r'\s*\{(.*?)\};', header, re.S)
    assert initializer, name
    return list(map(float, re.findall(r'-?\d+(?:\.\d+)?', initializer[1])))


def close(actual, expected):
    assert len(actual) == len(expected)
    assert all(math.isclose(a, b, abs_tol=0.00002) for a, b in zip(actual, expected)), (actual, expected)

close(values('LINK_LENGTHS_MM'), report['kinematics']['link_lengths_mm'])
close(values('MOTOR_REDUCTION'), report['nominal_drive']['motor_reduction'])
close(values('SCREW_LEAD_MM'), [report['nominal_drive']['screw_lead_mm']])
for constant, link in [('J1_FRAME_Z_MM', 'link_1'), ('J2_FRAME_Z_MM', 'link_2'), ('CARRIAGE_FRAME_Z_MM', 'link_3')]:
    close(values(constant), [report['visual_links'][link]['rest_translation_mm'][2]])
close(values('TOOL_TIP_LOCAL_Z_MM'), [report['kinematics']['tool_tip_local_mm'][2]])
close(values('REST_TCP_Z_MM'), [report['kinematics']['rest_tcp_mm'][2]])
close(values('HOME_SENSOR_MOUNTS'), [v for s in report['sensors'] for v in s['local_plunger_center_mm'] + s['outward_axis']])
for link in report['visual_links'].values():
    assert link['cross_check']['max_bound_residual_mm'] < .002
print('PASS: STEP/DAE hashes, body coverage, frame registration, C++ geometry and sensor constants')
