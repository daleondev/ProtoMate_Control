"""Compare reviewed HMI fixtures; never update the approved images automatically."""
import json
import os
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw
import pytest

BASELINE = Path(__file__).parent / "golden"
SPEC = json.loads((BASELINE / "regions.json").read_text())


@pytest.mark.parametrize("name", sorted(SPEC))
def test_reviewed_screen(name):
    directory = os.environ.get("HMI_CAPTURE_DIR")
    if not directory:
        pytest.skip("Run through CTest gui.screenshots to generate the presentation fixtures")
    directory = Path(directory)
    actual = Image.open(directory / f"{name}.bmp").convert("RGB")
    expected = Image.open(BASELINE / f"{name}.png").convert("RGB")
    assert actual.size == expected.size == (1024, 600)
    difference = ImageChops.difference(actual, expected)
    mask = Image.new("L", actual.size, 255)
    draw = ImageDraw.Draw(mask)
    for box in SPEC[name]["ignore"]:
        draw.rectangle(box, fill=0)
    for box in SPEC[name]["include"]:
        draw.rectangle(box, fill=255)
    difference = ImageChops.multiply(difference, mask.convert("RGB"))
    if difference.getbbox():
        difference.save(directory / f"{name}-difference.png")
        pytest.fail(f"Reviewed layout changed: {name}; inspect {name}-difference.png")


@pytest.mark.parametrize("name,box", [
    ("page-axis-jog", (456, 156, 176, 56)),  # tab
    ("page-axis-jog", (824, 284, 104, 104)),  # large circular control
    ("page-axis-move", (456, 292, 544, 56)),  # two-line numeric field
    ("gui-keypad", (200, 210, 120, 56)),
    ("page-cartesian", (456, 292, 120, 64)),
    ("page-home", (12, 12, 80, 92)),  # icon and caption centered as one group
    ("page-home", (12, 492, 80, 92)),
])
def test_caption_ink_is_centered(name, box):
    directory = os.environ.get("HMI_CAPTURE_DIR")
    if not directory:
        pytest.skip("Run through CTest gui.screenshots")
    image = Image.open(Path(directory) / f"{name}.bmp").convert("RGB")
    x, y, width, height = box
    ink = []
    for yy in range(8, height - 8):
        for xx in range(8, width - 8):
            pixel = image.getpixel((x + xx, y + yy))
            if min(pixel) > 150 and max(pixel) - min(pixel) < 60:
                ink.append((xx, yy))
    assert ink, f"Missing caption in {name} {box}"
    xs, ys = zip(*ink)
    # Font side bearings/descenders are not symmetric; allow three pixels
    # around the geometric centre while catching left-aligned or top-set text.
    assert abs((min(xs) + max(xs)) / 2 - width / 2) <= 3
    assert abs((min(ys) + max(ys)) / 2 - height / 2) <= 3
