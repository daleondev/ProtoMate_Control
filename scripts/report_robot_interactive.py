#!/usr/bin/env python3
"""Render and record the two robot detail levels using the actual TGX tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'src/ui/gui/assets/robot'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--compiled', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--captures', type=Path, default=ROOT / 'build/robot-interactive/captures')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SCENE3D_CAPTURE_DIR=str(args.captures.resolve()))
    output = subprocess.check_output([str(args.binary.resolve())], env=env, text=True)
    views = {int(i): float(iou) for i, iou in re.findall(r'LOD view=(\d+) silhouette_iou=([.\d]+)', output)}
    if len(views) != 36 or min(views.values()) <= .98:
        raise RuntimeError('Incomplete or unacceptable visual comparison')
    manifest = json.loads((ASSETS / 'interactive/manifest.json').read_text())
    compiled = {}
    for name, source in manifest['links'].items():
        result = json.loads((args.compiled / f'robot_interactive_{name}.json').read_text())
        if result['triangles'] != source['triangles'] or not result['watertight_enabled'] or result['textures']:
            raise RuntimeError('Compiled mesh does not match the source')
        compiled[name] = result
    for pose in range(3):
        sheet = Image.new('RGB', (1280, 6 * 220), '#202020')
        draw = ImageDraw.Draw(sheet)
        for local in range(12):
            index = pose * 12 + local
            x, y = (local % 2) * 640, (local // 2) * 220
            for col, name in enumerate(('full', 'interactive')):
                image = Image.open(args.captures / f'{name}-{index}.ppm').resize((320, 200))
                sheet.paste(image, (x + col * 320, y + 20))
                draw.text((x + col * 320 + 5, y + 4), f'{name}, view {index}', fill='white')
        sheet.save(args.output / f'pose-{pose}.png')
    comparison = Image.new('RGB', (1280, 430), '#202020')
    draw = ImageDraw.Draw(comparison)
    for col, name in enumerate(('full', 'interactive')):
        comparison.paste(Image.open(args.captures / f'{name}-0.ppm'), (col * 640, 30))
        draw.text((col * 640 + 10, 8), name, fill='white')
    comparison.save(args.output / 'comparison.png')
    report = dict(manifest_sha256=digest(ASSETS / 'interactive/manifest.json'),
                  full_manifest_sha256=digest(ASSETS / 'manifest.json'),
                  test_source_sha256=digest(ROOT / 'src/ui/gui/scene3d/tests/robot_assets_test.cpp'),
                  renderer_sha256=digest(ROOT / 'src/ui/gui/scene3d/Renderer.cpp'),
                  converter_sha256=digest(ROOT / 'scripts/scene3d_assets.py'),
                  script_sha256=digest(Path(__file__)),
                  minimum_silhouette_iou=min(views.values()), views=views, tgx=compiled)
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"36 matched views, minimum silhouette IoU {report['minimum_silhouette_iou']:.3%}")


if __name__ == '__main__':
    main()
