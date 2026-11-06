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
