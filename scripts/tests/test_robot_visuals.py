"""Asset invariants; normal builds need neither Blender nor a graphics context."""
import hashlib
import importlib.util
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'src/ui/gui/assets/robot'
SCRIPT = ROOT / 'scripts/prepare_robot_visuals.py'
spec = importlib.util.spec_from_file_location('prepare_robot_visuals', SCRIPT)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@pytest.mark.parametrize('name', prepare.LINKS)
def test_fingerprints_geometry_frames_and_normals(name):
    manifest = json.loads((ASSETS / 'manifest.json').read_text())
    record = manifest['links'][name]
    assert manifest['generator_sha256'] == sha(SCRIPT)
    assert manifest['units'] == 'metres' and manifest['up'] == 'Z'
    source = prepare.SOURCE / name / (name + '.dae')
    assert record['source_sha256'] == sha(source)
    assert record['sha256'] == sha(ASSETS / (name + '.obj'))
    assert record['mtl_sha256'] == sha(ASSETS / (name + '.mtl'))
    vertices, normals, faces = [], [], []
    for line in (ASSETS / (name + '.obj')).read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == 'v':
            vertices.append(list(map(float, fields[1:])))
        elif fields[0] == 'vn':
            normals.append(list(map(float, fields[1:])))
        elif fields[0] == 'f':
            assert len(fields) == 4
            face = [tuple(map(int, corner.split('//'))) for corner in fields[1:]]
            assert all(1 <= v <= len(vertices) and 1 <= n <= len(normals) for v, n in face)
            faces.append([v-1 for v, _ in face])
        assert fields[0] not in ('vt', 'map_Kd')
    points = np.asarray(vertices)
    normal = np.asarray(normals)
    assert np.isfinite(points).all() and np.isfinite(normal).all()
    assert np.allclose(np.linalg.norm(normal, axis=1), 1, atol=2e-6)
    triangles = points[np.asarray(faces)]
    areas = np.linalg.norm(np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0]), axis=1)
    assert (areas > 1e-12).all()
    assert len(faces) == record['triangles'] < record['original_triangles']
    assert np.allclose(points.min(axis=0), record['minimum'], atol=1e-8)
    assert np.allclose(points.max(axis=0), record['maximum'], atol=1e-8)
    original = prepare.read_link(source)
    original_points = np.asarray([v for b in original for v in b['vertices']])
    # Sub-millimetre bounds drift allows simplification, never unit changes,
    # recentering, axis swaps or losing the tool tip.
    assert np.allclose(points.min(axis=0), original_points.min(axis=0), atol=.001)
    assert np.allclose(points.max(axis=0), original_points.max(axis=0), atol=.001)
    assert record['max_sampled_surface_error_mm'] <= manifest['surface_error_mm']
    assert all('RX_' in b['name'] or 'Ruthex' in b['name'] for b in record['omitted_insert_bodies'])


@pytest.mark.parametrize('change', ['units', 'up', 'transform', 'nan'])
def test_import_rejects_unsupported_coordinates(tmp_path, change):
    ns = prepare.NS
    root = ET.parse(prepare.SOURCE / 'link_3/link_3.dae').getroot()
    if change == 'units':
        root.find('c:asset/c:unit', ns).set('meter', '.001')
    elif change == 'up':
        root.find('c:asset/c:up_axis', ns).text = 'Y_UP'
    elif change == 'transform':
        node = root.find('c:library_visual_scenes/c:visual_scene/c:node', ns)
        ET.SubElement(node, '{' + ns['c'] + '}translate').text = '1 2 3'
    else:
        array = root.find('c:library_geometries/c:geometry/c:mesh/c:source/c:float_array', ns)
        array.text = 'nan ' + array.text.split(maxsplit=1)[1]
    path = tmp_path / 'bad.dae'
    ET.ElementTree(root).write(path)
    with pytest.raises(ValueError):
        prepare.read_link(path)


def test_review_matches_assets_and_quantization_preserves_geometry():
    report = json.loads((ROOT / 'docs/robot/visuals/validation.json').read_text())
    manifest = json.loads((ASSETS / 'manifest.json').read_text())
    assert report['asset_manifest_sha256'] == sha(ASSETS / 'manifest.json')
    assert report['converter_sha256'] == sha(ROOT / 'scripts/scene3d_assets.py')
    assert report['capture_script_sha256'] == sha(ROOT / 'scripts/review_robot_visuals.py')
    assert len(report['views']) == 36
    assert report['minimum_silhouette_iou'] > .995
    for name, mesh in report['tgx'].items():
        assert mesh['triangles'] == manifest['links'][name]['triangles']
        assert mesh['watertight_enabled'] and mesh['textures'] == 0
        # Quantization is a separate budget from visual simplification.
        assert mesh['watertight_max_displacement'] < .00002
        assert mesh['watertight_max_delta32'] < .0000001
