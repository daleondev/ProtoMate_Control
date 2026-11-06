#!/usr/bin/env python3
"""Compare the 36 matched Blender renders and retain review contact sheets."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--views', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/robot/visuals')
    parser.add_argument('--compiled', type=Path)
    parser.add_argument('--tgx-captures', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = ROOT / 'src/ui/gui/assets/robot/manifest.json'
    manifest = json.loads(manifest_path.read_text())
    records = []
    for pose in range(3):
        for pitch in (25, -25, 75):
            canvas = Image.new('RGB', (1280, 450), '#eceff3')
            draw = ImageDraw.Draw(canvas)
            for column, azimuth in enumerate((0, 90, 180, 270)):
                images, paths = [], []
                view = f'pose{pose}-a{azimuth}-p{pitch}'
                for v, variant in enumerate(('original', 'optimized')):
                    path = args.views / f'{view}-{variant}.png'
                    paths.append(path)
                    rgba = Image.open(path).convert('RGBA')
                    if rgba.size != (640, 400):
                        raise ValueError(f'Unexpected capture size: {path}')
                    images.append(np.asarray(rgba))
                    background = Image.new('RGBA', rgba.size, '#eceff3')
                    background.alpha_composite(rgba)
                    x, y = column % 2 * 640 + v * 320, column // 2 * 225 + 25
                    canvas.paste(background.convert('RGB').resize((320, 200)), (x, y))
                    draw.text((x + 5, y - 20), f'{variant} | az {azimuth} | pitch {pitch}', fill='black')
                a, b = images
                ma, mb = a[:, :, 3] > 127, b[:, :, 3] > 127
                union = ma | mb
                if not union.any():
                    raise ValueError(f'Empty captures: {view}')
                records.append(dict(view=view, original_sha256=sha(paths[0]), optimized_sha256=sha(paths[1]),
                                    silhouette_iou=float(np.count_nonzero(ma & mb) / np.count_nonzero(union)),
                                    mean_rgb_error_on_union=float(np.abs(a[:, :, :3].astype(float) -
                                                                        b[:, :, :3]).mean(axis=2)[union].mean())))
            canvas.save(args.output / f'pose{pose}-pitch{pitch}.png')
    report = dict(asset_manifest_sha256=sha(manifest_path),
                  preparation_sha256=manifest['generator_sha256'],
                  capture_script_sha256=sha(ROOT / 'scripts/review_robot_visuals.py'),
                  converter_sha256=sha(ROOT / 'scripts/scene3d_assets.py'),
                  original_triangles=sum(v['original_triangles'] for v in manifest['links'].values()),
                  optimized_triangles=sum(v['triangles'] for v in manifest['links'].values()),
                  minimum_silhouette_iou=min(v['silhouette_iou'] for v in records), views=records)
    if args.compiled:
        report['tgx'] = {name: json.loads((args.compiled / f'robot_{name}.json').read_text())
                         for name in manifest['links']}
    if args.tgx_captures:
        report['host_tgx_captures'] = {}
        for index in range(3):
            source = args.tgx_captures / f'robot-{index}.ppm'
            target = args.output / f'robot-{index}-tgx.png'
            Image.open(source).save(target)
            report['host_tgx_captures'][target.name] = sha(target)
    (args.output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"36 matched views; worst silhouette IoU {report['minimum_silhouette_iou']:.6f}; "
          f"{report['original_triangles']} -> {report['optimized_triangles']} triangles")


if __name__ == '__main__':
    main()
