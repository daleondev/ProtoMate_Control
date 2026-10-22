"""Source drill centre lines for round holes and straight plated slots, in mm."""
import math
import pcbnew as k


def pad_drill(pad):
    """Return (start, end, cutter diameter); points use PCB coordinates."""
    dx, dy = (k.ToMM(v) for v in (pad.GetDrillSize().x, pad.GetDrillSize().y))
    x, y = k.ToMM(pad.GetPosition().x), k.ToMM(pad.GetPosition().y)
    length = abs(dx - dy) / 2
    angle = math.radians(-pad.GetOrientationDegrees() + (90 if dy > dx else 0))
    vx, vy = length * math.cos(angle), length * math.sin(angle)
    return (x-vx, y-vy), (x+vx, y+vy), min(dx, dy)


def canonical_drill(start, end, diameter, plated, digits=6):
    a, b = sorted(tuple(round(v, digits) for v in p) for p in (start, end))
    return (*a, *b, round(diameter, digits), plated)
