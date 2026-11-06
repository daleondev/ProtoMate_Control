#!/usr/bin/env python3
"""Offline visual-only derivatives of the Fusion links (Blender 4.0.2).

Run with Blender, not the firmware Python asset converter. Coordinates stay in
metres/Z-up in the original link frames. Never modifies CAD/contact assets.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/simulation/assets/robot'
DEST = ROOT / 'src/ui/gui/assets/robot'
LINKS = ('base_link', 'link_1', 'link_2', 'link_3')
NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_link(path):
    """Strictly read the baked Fusion subset; fail rather than lose transforms."""
    root = ET.parse(path).getroot()
    if (root.find('c:asset/c:unit', NS).get('meter') != '1' or
            root.findtext('c:asset/c:up_axis', namespaces=NS) != 'Z_UP'):
        raise ValueError('Expected metre/Z-up coordinates')
    effects = {}
    for effect in root.findall('c:library_effects/c:effect', NS):
        color = effect.find('.//c:diffuse/c:color', NS)
        rgba = tuple(map(float, color.text.split()))
        if len(rgba) != 4 or rgba[3] != 1 or not all(math.isfinite(x) for x in rgba):
            raise ValueError('Expected finite opaque material')
        effects[effect.get('id')] = rgba[:3]
    materials = {}
    for m in root.findall('c:library_materials/c:material', NS):
        materials[m.get('id')] = effects[m.find('c:instance_effect', NS).get('url')[1:]]
    geometries = {}
    for geometry in root.findall('c:library_geometries/c:geometry', NS):
        mesh = geometry.find('c:mesh', NS)
        pos = mesh.find('c:vertices/c:input[@semantic="POSITION"]', NS).get('source')[1:]
        values = list(map(float, mesh.find(f'c:source[@id="{pos}"]/c:float_array', NS).text.split()))
        if len(values) % 3 or not all(math.isfinite(v) for v in values):
            raise ValueError('Invalid vertices')
        vertices = list(zip(values[::3], values[1::3], values[2::3]))
        faces, colors = [], []
        for group in mesh.findall('c:triangles', NS):
            inputs = group.findall('c:input', NS)
            stride = 1 + max(int(i.get('offset')) for i in inputs)
            offset = int(group.find('c:input[@semantic="VERTEX"]', NS).get('offset'))
            raw = list(map(int, group.find('c:p', NS).text.split()))
            if len(raw) != int(group.get('count')) * 3 * stride:
                raise ValueError('Invalid triangle data')
            ids = raw[offset::stride]
            if any(i < 0 or i >= len(vertices) for i in ids):
                raise ValueError('Invalid vertex index')
            faces.extend(zip(ids[::3], ids[1::3], ids[2::3]))
            colors.extend([group.get('material')] * (len(ids) // 3))
        if not faces or mesh.find('c:polylist', NS) is not None:
            raise ValueError('Expected triangulated mesh')
        geometries[geometry.get('id')] = vertices, faces, colors
    bodies = []
    for node in root.findall('c:library_visual_scenes/c:visual_scene/c:node', NS):
        if any(node.find('c:' + t, NS) is not None for t in
               ('matrix', 'translate', 'rotate', 'scale', 'node', 'instance_node')):
            raise ValueError('Expected baked link-local geometry')
        instance = node.find('c:instance_geometry', NS)
        bind = {m.get('symbol'): m.get('target')[1:] for m in
                instance.findall('.//c:instance_material', NS)}
        vertices, faces, symbols = geometries[instance.get('url')[1:]]
        bodies.append(dict(name=node.get('name'), vertices=vertices, faces=faces,
                           colors=[materials[bind[s]] for s in symbols]))
    return bodies


def make_objects(bodies, label):
    import bpy
    result = []
    palette = {}
    for number, body in enumerate(bodies):
        mesh = bpy.data.meshes.new(f'{label}_{number}')
        mesh.from_pydata(body['vertices'], [], body['faces'])
        mesh.update()
        colors = list(dict.fromkeys(body['colors']))
        for rgb in colors:
            if rgb not in palette:
                material = bpy.data.materials.new('rgb_' + '_'.join(f'{x:.6f}' for x in rgb))
                material.diffuse_color = (*rgb, 1)
                palette[rgb] = material
            mesh.materials.append(palette[rgb])
        for face, color in zip(mesh.polygons, body['colors']):
            face.material_index = colors.index(color)
            face.use_smooth = True
        mesh.use_auto_smooth = True
        mesh.auto_smooth_angle = math.radians(35)
        obj = bpy.data.objects.new(f'{label}_{number}', mesh)
        obj['source_body'] = body['name']
        bpy.context.collection.objects.link(obj)
        result.append(obj)
    return result


def apply_modifier(obj, modifier):
    import bpy
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def mesh_bvh(mesh):
    from mathutils.bvhtree import BVHTree
    mesh.calc_loop_triangles()
    return BVHTree.FromPolygons([v.co for v in mesh.vertices],
                               [tuple(t.vertices) for t in mesh.loop_triangles], all_triangles=True)


def sample_points(mesh):
    from mathutils import Vector
    mesh.calc_loop_triangles()
    # Deterministic coverage, including thin features and face interiors.
    vertices = list(mesh.vertices)
    triangles = list(mesh.loop_triangles)
    return ([v.co.copy() for v in vertices[::max(1, len(vertices)//1500)]] +
            [sum((mesh.vertices[i].co for i in t.vertices), Vector())/3
             for t in triangles[::max(1, len(triangles)//1500)]])


def transfer_normals(obj, source, bvh):
    from mathutils.geometry import barycentric_transform
    source.calc_normals_split()
    source.calc_loop_triangles()
    normals = [None] * len(obj.data.loops)
    for face in obj.data.polygons:
        for loop_index in face.loop_indices:
            vertex = obj.data.vertices[obj.data.loops[loop_index].vertex_index].co
            point = vertex.lerp(face.center, .001)
            location, _, triangle, _ = bvh.find_nearest(point)
            tri = source.loop_triangles[triangle]
            a, b, c = (source.vertices[i].co for i in tri.vertices)
            na, nb, nc = (source.loops[i].normal for i in tri.loops)
            normal = barycentric_transform(location, a, b, c, na, nb, nc).normalized()
            if normal.dot(face.normal) < math.cos(math.radians(40)):
                normal = face.normal
            normals[loop_index] = normal
    obj.data.normals_split_custom_set(normals)


def simplify(objects, ratio, error_mm):
    import bpy
    for obj in objects:
        if not obj.data.polygons:
            obj['sampled_surface_error_mm'] = 0
            continue
        source = obj.data.copy()
        reference = mesh_bvh(source)
        samples = sample_points(source)
        # Weld only within a CAD body, never across a moving joint or material.
        planar = obj.modifiers.new('Coplanar faces', 'DECIMATE')
        planar.decimate_type = 'DISSOLVE'
        planar.angle_limit = math.radians(0.5)
        planar.delimit = {'MATERIAL', 'SHARP', 'NORMAL'}
        apply_modifier(obj, planar)
        obj.data.calc_loop_triangles()
        count = len(obj.data.loop_triangles)
        target = max(12, int(count * ratio))
        planar_mesh = obj.data.copy()
        tolerance = error_mm / 1000
        accepted_error = 0
        # Bounded attempts; never force a count at the expense of the shape.
        for fraction in (target/count, .25, .4, .6, .8, 1.0):
            if fraction < target/count:
                continue
            previous = obj.data
            obj.data = planar_mesh.copy()
            bpy.data.meshes.remove(previous)
            if fraction < 1:
                decimate = obj.modifiers.new('Visual simplification', 'DECIMATE')
                decimate.ratio = fraction
                decimate.use_collapse_triangulate = True
                apply_modifier(obj, decimate)
            candidate = mesh_bvh(obj.data)
            error_out = max(reference.find_nearest(p)[3] for p in sample_points(obj.data))
            error_in = max(candidate.find_nearest(p)[3] for p in samples)
            accepted_error = max(error_in, error_out)
            if accepted_error <= tolerance:
                break
        if accepted_error > tolerance:
            previous = obj.data
            obj.data = source.copy()
            bpy.data.meshes.remove(previous)
            accepted_error = 0
        obj['sampled_surface_error_mm'] = accepted_error * 1000
        triangulate = obj.modifiers.new('Export triangles', 'TRIANGULATE')
        apply_modifier(obj, triangulate)
        bpy.data.meshes.remove(planar_mesh)
        bpy.data.meshes.remove(source)


def exterior(objects, directions=48):
    """Conservative sampled visibility within one rigid link, never an assembly.

    Keep a face if any of four interior sample points has a clear outward ray.
    This is a visual simplification heuristic, not a proof for all sightlines.
    Retain one adjacent face ring to protect cavity edges and narrow openings.
    """
    import bmesh
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    vertices, faces = [], []
    for obj in objects:
        offset = len(vertices)
        vertices.extend(v.co.copy() for v in obj.data.vertices)
        faces.extend(tuple(offset + i for i in p.vertices) for p in obj.data.polygons)
    bvh = BVHTree.FromPolygons(vertices, faces, all_triangles=True, epsilon=0)
    sphere = []
    for i in range(directions):
        z = 1 - 2 * (i + .5) / directions
        phi = i * math.pi * (3 - math.sqrt(5))
        sphere.append(Vector((math.sqrt(1-z*z)*math.cos(phi), math.sqrt(1-z*z)*math.sin(phi), z)))
    before = len(faces)
    for obj in objects:
        mesh = obj.data
        keep = set()
        for face in mesh.polygons:
            a, b, c = (mesh.vertices[i].co for i in face.vertices)
            normal = face.normal
            rays = [normal] + [d for d in sphere if d.dot(normal) > .05]
            for point in ((a+b+c)/3, .8*a+.1*b+.1*c, .1*a+.8*b+.1*c, .1*a+.1*b+.8*c):
                start = point + normal * 0.0000002
                if any(bvh.ray_cast(start, d)[0] is None for d in rays):
                    keep.add(face.index)
                    break
        bm = bmesh.new()
        bm.from_mesh(mesh)
        bm.faces.ensure_lookup_table()
        fringe = set(keep)
        for index in keep:
            for edge in bm.faces[index].edges:
                fringe.update(f.index for f in edge.link_faces)
        bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.index not in fringe], context='FACES')
        bm.to_mesh(mesh)
        bm.free()
        mesh.update()
    return before - sum(len(o.data.polygons) for o in objects)


def write_obj(objects, path):
    """Write explicit per-corner normals and unmodified local coordinates."""
    palette = sorted({tuple(m.diffuse_color[:3]) for o in objects for m in o.data.materials})
    mtl = path.with_suffix('.mtl')
    mtl.write_text('# Source material colours; opaque, no textures.\n' + ''.join(
        f'newmtl color_{i}\nKa 0.25 0.25 0.25\nKd {r:.9g} {g:.9g} {b:.9g}\nKs 0.08 0.08 0.08\nNs 16\nd 1\n\n'
        for i, (r,g,b) in enumerate(palette)))
    lines = ['# Visual derivative only. Metres, right-handed Z-up, original link origin.', f'mtllib {mtl.name}']
    vertex_base, normal_base = 1, 1
    triangles = 0
    all_points = []
    for obj in objects:
        mesh = obj.data
        if not mesh.polygons:
            continue
        mesh.calc_normals_split()
        mesh.calc_loop_triangles()
        used = sorted({i for tri in mesh.loop_triangles for i in tri.vertices})
        remap = {index: i + vertex_base for i, index in enumerate(used)}
        points = [mesh.vertices[i].co for i in used]
        all_points.extend(tuple(p) for p in points)
        lines.append('g ' + obj.name)
        lines.extend('v ' + ' '.join(f'{v:.9g}' for v in p) for p in points)
        normals = {}
        for tri in mesh.loop_triangles:
            for loop in tri.loops:
                n = tuple(round(v, 7) for v in mesh.loops[loop].normal)
                if n not in normals:
                    normals[n] = normal_base + len(normals)
                    lines.append('vn ' + ' '.join(f'{v:.9g}' for v in n))
        last_material = None
        for tri in mesh.loop_triangles:
            a, b, c = (mesh.vertices[i].co for i in tri.vertices)
            if (b-a).cross(c-a).length_squared < 1e-24:
                continue
            material = palette.index(tuple(mesh.materials[tri.material_index].diffuse_color[:3]))
            if material != last_material:
                lines.append(f'usemtl color_{material}')
                last_material = material
            corners = []
            for v, loop in zip(tri.vertices, tri.loops):
                n = tuple(round(x, 7) for x in mesh.loops[loop].normal)
                corners.append(f'{remap[v]}//{normals[n]}')
            lines.append('f ' + ' '.join(corners))
            triangles += 1
        vertex_base += len(used)
        normal_base += len(normals)
    path.write_text('\n'.join(lines) + '\n')
    return dict(triangles=triangles, vertices=vertex_base-1, materials=len(palette),
                minimum=[min(p[i] for p in all_points) for i in range(3)],
                maximum=[max(p[i] for p in all_points) for i in range(3)],
                sha256=digest(path), mtl_sha256=digest(mtl))


def main():
    import bpy
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DEST)
    parser.add_argument('--ratio', type=float, default=.12)
    parser.add_argument('--error-mm', type=float, default=.75)
    parser.add_argument('--link', choices=LINKS, action='append')
    parser.add_argument('--blend', type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else [])
    if not 0 < args.ratio <= 1 or not math.isfinite(args.error_mm) or args.error_mm <= 0:
        parser.error('ratio must be in (0, 1] and error-mm must be finite and positive')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(generator_sha256=digest(Path(__file__)), blender=bpy.app.version_string,
                  units='metres', up='Z', ratio=args.ratio, surface_error_mm=args.error_mm, links={})
    for name in args.link or LINKS:
        path = SOURCE / name / (name+'.dae')
        bodies = read_link(path)
        original = make_objects(bodies, name+'_original')
        # Deliberately suppress tiny threaded brass inserts, not visible screw
        # heads, switch levers, bearings, structural openings or the tool tip.
        omitted = [b for b in bodies if 'RX_' in b['name'] or 'Ruthex' in b['name']]
        optimized = make_objects([b for b in bodies if b not in omitted], name+'_optimized')
        import bmesh
        for obj in optimized:
            bm = bmesh.new()
            bm.from_mesh(obj.data)
            bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=0.0000001)
            bm.to_mesh(obj.data)
            bm.free()
            obj.data.update()
        simplify(optimized, args.ratio, args.error_mm)
        # Simplify closed bodies before visibility pruning; otherwise open
        # boundaries constrain edge collapses and increase the final count.
        removed = exterior(optimized)
        originals = {o['source_body']: o for o in original}
        for obj in optimized:
            if obj.data.polygons:
                source = originals[obj['source_body']].data
                transfer_normals(obj, source, mesh_bvh(source))
        record = write_obj(optimized, args.output / (name+'.obj'))
        record.update(source_sha256=digest(path), original_triangles=sum(len(b['faces']) for b in bodies),
                      hidden_triangles_removed=removed,
                      max_sampled_surface_error_mm=max(o['sampled_surface_error_mm'] for o in optimized),
                      omitted_insert_bodies=[dict(name=b['name'], triangles=len(b['faces'])) for b in omitted])
        report['links'][name] = record
        print(f"{name}: {record['original_triangles']} -> {record['triangles']} triangles; "
              f"{removed} hidden faces removed", flush=True)
        for o in original:
            o.hide_render = True
            o.hide_set(True)
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    if args.blend:
        args.blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))


if __name__ == '__main__':
    main()
