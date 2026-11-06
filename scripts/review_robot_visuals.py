#!/usr/bin/env python3
"""Blender: render matched CAD/derivative views without modifying saved assets."""
import argparse
import math
from pathlib import Path
import sys

import bpy
from mathutils import Matrix, Vector


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--quick', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    args.output.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.color_type = 'MATERIAL'
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = False
    scene.display.shading.show_specular_highlight = False
    scene.render.resolution_x, scene.render.resolution_y = 640, 400
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = True
    scene.view_settings.view_transform = 'Standard'
    scene.render.image_settings.file_format = 'PNG'
    camera = bpy.data.objects.new('ReviewCamera', bpy.data.cameras.new('ReviewCamera'))
    scene.collection.objects.link(camera)
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = .95
    camera.data.clip_start, camera.data.clip_end = .001, 10
    scene.camera = camera
    links = ('base_link', 'link_1', 'link_2', 'link_3')
    poses = [(0, 0, 0), (40, -100, .04), (-70, 120, -.02)]
    views = [(a, p) for p in (25, -25, 75) for a in (0, 90, 180, 270)]
    if args.quick:
        poses, views = poses[:1], [(35, 25)]
    for pose_index, (q1, q2, z) in enumerate(poses):
        matrices = [Matrix.Identity(4)]
        matrices.append(Matrix.Translation((0, 0, .052)) @ Matrix.Rotation(math.radians(q1), 4, 'Z'))
        matrices.append(matrices[1] @ Matrix.Translation((.205, 0, .3179)) @ Matrix.Rotation(math.radians(q2), 4, 'Z'))
        matrices.append(matrices[2] @ Matrix.Translation((.3584, 0, -.061 + z)))
        for obj in bpy.data.objects:
            if obj.type == 'MESH':
                link = next(i for i, name in enumerate(links) if obj.name.startswith(name))
                obj.matrix_world = matrices[link]
        points = [o.matrix_world @ Vector(c) for o in bpy.data.objects
                  if o.type == 'MESH' and '_original_' in o.name for c in o.bound_box]
        target = (Vector(tuple(min(p[i] for p in points) for i in range(3))) +
                  Vector(tuple(max(p[i] for p in points) for i in range(3)))) / 2
        for azimuth, pitch in views:
            a, p = math.radians(azimuth), math.radians(pitch)
            camera.location = target + Vector((math.sin(a)*math.cos(p), -math.cos(a)*math.cos(p), math.sin(p))) * 2
            camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
            for variant in ('original', 'optimized'):
                for obj in bpy.data.objects:
                    if obj.type == 'MESH':
                        visible = f'_{variant}_' in obj.name
                        obj.hide_render = not visible
                        obj.hide_set(not visible)
                scene.render.filepath = str(args.output / f'pose{pose_index}-a{azimuth}-p{pitch}-{variant}.png')
                bpy.ops.render.render(write_still=True)


if __name__ == '__main__':
    main()
