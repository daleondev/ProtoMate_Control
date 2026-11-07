#!/usr/bin/env python3
"""Generate static HMI axis illustrations from the actual TGX/CAD renderer."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'src/ui/gui/assets/images')
    parser.add_argument('--work', type=Path, default=ROOT / 'build/hmi-modern/illustrations')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [str(args.binary.resolve()),
         '--gtest_filter=RobotVisuals.AxisIllustrationsUseTheRegisteredDetailedMesh'],
        env=dict(os.environ, SCENE3D_AXIS_DIR=str(args.work.resolve())), check=True)
    report = {}
    for index, name in enumerate(('shoulder', 'elbow', 'z')):
        image = Image.open(args.work / f'axis-{index}.ppm').convert('RGB')
        # The direction guide follows points projected by the same TGX matrices.
        points = [tuple(map(float, line.split()))
                  for line in (args.work / f'axis-{index}.txt').read_text().splitlines()]
        draw = ImageDraw.Draw(image)
        draw.line(points, fill='#2587f4', width=9, joint='curve')
        x, y = points[-1]
        px, py = points[-2]
        angle = math.atan2(y - py, x - px)
        draw.polygon([(x, y),
                      (x - 21 * math.cos(angle - .5), y - 21 * math.sin(angle - .5)),
                      (x - 21 * math.cos(angle + .5), y - 21 * math.sin(angle + .5))],
                     fill='#56b6ff')
        image = image.resize((304, 190), Image.Resampling.LANCZOS)
        path = args.output / f'axis_{name}.png'
        image.save(path)
        report[name] = {
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'size': [304, 190],
            'positive_direction': ('joint-local +Z' if index == 2
                                   else 'joint-local positive Z rotation'),
            'projected_guide': points,
        }
    (args.output.parent / 'axis-illustrations.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
