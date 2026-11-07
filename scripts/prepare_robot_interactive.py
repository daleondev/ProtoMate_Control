#!/usr/bin/env python3
"""Create a motion-only visual LOD from the source CAD bodies (Blender 4.0.2).

No CAD, contact mesh, link origin or full-detail asset is changed. Normal builds
consume the checked-in OBJ/MTL files and do not require Blender.
"""
import argparse
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_robot_visuals as full


def read_obj(path):
    """Read our explicit triangle/corner-normal OBJ without axis conversions."""
    colors = {}
    material = None
    for line in path.with_suffix('.mtl').read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == 'newmtl':
            material = fields[1]
        elif fields and fields[0] == 'Kd':
            colors[material] = tuple(map(float, fields[1:]))
    vertices, normals, groups = [], [], []
    for line in path.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith('#'):
            continue
        if fields[0] == 'v':
            vertices.append(tuple(map(float, fields[1:])))
        elif fields[0] == 'vn':
            normals.append(tuple(map(float, fields[1:])))
        elif fields[0] == 'g':
            groups.append(dict(name=fields[1], faces=[], colors=[], normals=[]))
        elif fields[0] == 'usemtl':
            material = fields[1]
        elif fields[0] == 'f':
            if len(fields) != 4:
                raise ValueError('Expected prepared triangles')
            corners = [tuple(int(x)-1 for x in c.split('//')) for c in fields[1:]]
            groups[-1]['faces'].append(tuple(c[0] for c in corners))
            groups[-1]['normals'].extend(normals[c[1]] for c in corners)
            groups[-1]['colors'].append(colors[material])
    for group in groups:
        used = sorted({i for f in group['faces'] for i in f})
        remap = {i: n for n, i in enumerate(used)}
        group['vertices'] = [vertices[i] for i in used]
        group['faces'] = [tuple(remap[i] for i in f) for f in group['faces']]
    return groups


def main():
    import bpy
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=full.DEST / 'interactive')
    parser.add_argument('--triangles', type=int, default=8000)
    parser.add_argument('--voxel-mm', type=float, default=2)
    parser.add_argument('--omit-below-mm', type=float, default=8)
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else [])
    if bpy.app.version[:3] != (4, 0, 2):
        raise RuntimeError('Use Blender 4.0.2 for reproducible modifiers and normals')
    if not 2000 <= args.triangles <= 20000 or not 1 <= args.voxel_mm <= 4 or not 0 <= args.omit_below_mm <= 10:
        parser.error('Invalid simplification settings')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(generator_sha256=full.digest(Path(__file__)),
                  helper_sha256=full.digest(Path(full.__file__)), blender=bpy.app.version_string,
                  units='metres', up='Z', triangles=args.triangles, voxel_mm=args.voxel_mm,
                  omit_below_mm=args.omit_below_mm, links={})
    for name in full.LINKS:
        path = full.DEST / (name + '.obj')
        cad = full.SOURCE / name / (name + '.dae')
        bodies = full.read_link(cad)
        retained, omitted = [], []
        for body in bodies:
            extent = [max(p[i] for p in body['vertices']) - min(p[i] for p in body['vertices'])
                      for i in range(3)]
            diameter = math.sqrt(sum(v*v for v in extent)) * 1000
            if diameter < args.omit_below_mm or 'RX_' in body['name'] or 'Ruthex' in body['name']:
                omitted.append(dict(name=body['name'], triangles=len(body['faces']), diameter_mm=diameter))
            else:
                retained.append(body)
        # A voxel union removes small cavities and gives decimation a regular,
        # closed surface. Collapsing the CAD's long, thin triangles directly
        # damages the silhouette. Every link is processed independently.
        tip = [body for body in retained if 'cutter reference' in body['name']]
        objects = full.make_objects([body for body in retained if body not in tip], name + '_interactive')
        bpy.ops.object.select_all(action='DESELECT')
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]
        bpy.ops.object.join()
        obj = bpy.context.view_layer.objects.active
        source = obj.data.copy()
        reference = full.mesh_bvh(source)
        modifier = obj.modifiers.new('Visual shell union', 'REMESH')
        modifier.mode = 'VOXEL'
        modifier.voxel_size = args.voxel_mm / 1000
        modifier.use_smooth_shade = True
        full.apply_modifier(obj, modifier)
        triangulate = obj.modifiers.new('Triangles', 'TRIANGULATE')
        full.apply_modifier(obj, triangulate)
        target = int(args.triangles * dict(base_link=.30, link_1=.23, link_2=.35, link_3=.12)[name])
        modifier = obj.modifiers.new('Motion surface', 'DECIMATE')
        modifier.ratio = min(1, target / len(obj.data.polygons))
        modifier.use_collapse_triangulate = True
        full.apply_modifier(obj, modifier)
        # Transfer source colours after reduction, so invisible CAD material
        # partitions do not consume the triangle budget. Never export textures.
        source.calc_loop_triangles()
        for face in obj.data.polygons:
            _, _, nearest, _ = reference.find_nearest(face.center)
            face.material_index = source.loop_triangles[nearest].material_index
            face.use_smooth = True
        obj.data.use_auto_smooth = True
        obj.data.auto_smooth_angle = math.radians(35)
        # Smooth the new surface normally; old corner normals belong to the
        # high-detail topology and must not be pasted onto chamfered surfaces.
        candidate = full.mesh_bvh(obj.data)
        obj['sampled_surface_error_mm'] = 1000 * max(
            max(reference.find_nearest(p)[3] for p in full.sample_points(obj.data)),
            max(candidate.find_nearest(p)[3] for p in full.sample_points(source)))
        bpy.data.meshes.remove(source)
        objects = [obj] + full.make_objects(tip, name + '_tip')
        removed = full.exterior(objects)
        record = full.write_obj(objects, args.output / (name + '.obj'))
        record.update(source_sha256=full.digest(path),
                      source_mtl_sha256=full.digest(path.with_suffix('.mtl')),
                      cad_sha256=full.digest(cad), hidden_triangles_removed=removed,
                      original_triangles=sum(len(b['faces']) for b in bodies),
                      omitted_bodies=omitted,
                      max_sampled_surface_error_mm=max(o.get('sampled_surface_error_mm', 0) for o in objects))
        report['links'][name] = record
        print(f"{name}: {record['original_triangles']} -> {record['triangles']}; "
              f"omitted {len(omitted)} small bodies", flush=True)
        for obj in objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
