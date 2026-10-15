#!/usr/bin/env python3
"""Compile opaque OBJ/MTL assets with the pinned TGX meshlet exporter.

Uses TGX's conversion modules directly: only NumPy is needed, without the
upstream GUI/VTK/strip-solver installation. No normalization or texture export.
"""
import argparse
import json
import math
from pathlib import Path
import sys


def sphere_obj(path, sectors, bands):
    """Closed ellipsoid with exactly 2*sectors*(bands-1) triangles."""
    points = [(0., 0., 1.)]
    for ring in range(1, bands):
        phi = math.pi * ring / bands
        for j in range(sectors):
            theta = math.tau * j / sectors
            points.append((math.sin(phi)*math.cos(theta), math.sin(phi)*math.sin(theta), math.cos(phi)))
    points.append((0., 0., -1.))
    faces = []
    for j in range(sectors):
        faces.append((0, 1+j, 1+(j+1) % sectors))
    for ring in range(bands-2):
        for j in range(sectors):
            a = 1+ring*sectors+j
            b = 1+ring*sectors+(j+1) % sectors
            faces.extend(((a, a+sectors, b), (b, a+sectors, b+sectors)))
    last = len(points)-1
    for j in range(sectors):
        faces.append((last, last-sectors+(j+1) % sectors, last-sectors+j))
    lines = ['# Generated benchmark ellipsoid; local origin and units preserved.']
    lines += [f'v {x:.9g} {y*.65:.9g} {z*.8:.9g}' for x, y, z in points]
    for x, y, z in points:
        n = (x, y/.65, z/.8)
        length = math.sqrt(sum(v*v for v in n))
        lines.append('vn ' + ' '.join(f'{v/length:.9g}' for v in n))
    lines += ['f ' + ' '.join(f'{i+1}//{i+1}' for i in face) for face in faces]
    path.write_text('\n'.join(lines)+'\n')


def convert(tgx, source, output, name):
    import numpy as np
    sys.path.insert(0, str(tgx / 'tools'))
    from _internal.modules.mesh_pipeline.obj_loader import load_obj
    from _internal.modules.mesh_pipeline.mesh import FaceVertex, Triangle
    from _internal.modules.mesh_pipeline.preprocess import preprocess_mesh
    from _internal.modules.mesh_pipeline.meshlets import build_meshlets, MeshletBuildOptions
    from _internal.modules.mesh_pipeline.exporter import export_mesh3dv2_header
    mesh = load_obj(source)
    if not len(mesh.triangles) or len(mesh.triangles) > 65535:
        raise ValueError('Mesh needs 1..65535 triangles')
    if not np.isfinite(mesh.vertices).all() or not np.isfinite(mesh.normals).all():
        raise ValueError('Mesh contains nonfinite coordinates/normals')
    # Drop UVs and image references before conversion, including MTL maps.
    mesh.texcoords = np.empty((0, 2))
    mesh.triangles = [Triangle(tuple(FaceVertex(c.v, -1, c.vn) for c in t.corners),
                               t.material, t.group) for t in mesh.triangles]
    for material in mesh.materials.values():
        material.texture_path = material.emissive_texture_path = None
        material.texture_refs.clear()
        material.material_extra_present = False
    mesh, stats = preprocess_mesh(mesh, normalize=False, force_normals=not mesh.has_normals())
    if stats.degenerate_triangles_removed:
        raise ValueError('Degenerate triangles must be corrected in the source asset')
    if not mesh.has_normals():
        raise ValueError('Shaded meshes require normals')
    meshlets = build_meshlets(mesh, MeshletBuildOptions(target_vertices=30, max_triangles=64))
    result = export_mesh3dv2_header(mesh, meshlets, name=name, use_lkh=False,
                                   texture_symbols={}, emissive_texture_symbols={},
                                   header_filename=output.name, single_header=False)
    output.write_text(result.header)
    output.with_suffix('.cpp').write_text(result.source)
    lo, hi = mesh.bounding_box()
    report = dict(result.stats.__dict__, name=name, minimum=lo.tolist(), maximum=hi.tolist(), textures=0)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'{name}: {len(mesh.triangles)} triangles, {len(meshlets)} meshlets, '
          f'{result.stats.static_bytes} bytes, no textures', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tgx', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--name', default='model')
    parser.add_argument('--benchmark', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.benchmark:
        for name, sectors, bands in [('low', 10, 6), ('medium', 20, 11), ('high', 25, 21)]:
            source = args.output / (name+'.obj')
            sphere_obj(source, sectors, bands)
            convert(args.tgx.resolve(), source, args.output/(name+'.h'), name)
    else:
        if not args.input:
            parser.error('--input is required without --benchmark')
        convert(args.tgx.resolve(), args.input, args.output/(args.name+'.h'), args.name)


if __name__ == '__main__':
    main()
