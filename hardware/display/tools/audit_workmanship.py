"""Find routing-workmanship issues on ALL nets; never silently approve a layout.

Errors violate the common layout contract. Review findings require inspection
and a specific disposition in the hash-bound visual review. Short segments and
power/thermal redundancy cannot be judged by a length/count heuristic alone.
"""
import argparse
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path

import pcbnew as k
from shapely.geometry import LineString, Point
from shapely.ops import unary_union
from shapely.strtree import STRtree

from audit_copper import polygon_set
from layout_policy import (ANGLE_TOLERANCE_DEG, SHORT_SEGMENT_WIDTHS,
                           REFERENCE_LAYERS, COPPER_LAYERS, sha256)

k.SwigPyIterator.next = k.SwigPyIterator.__next__
EPS = .002


def xy(p):
    return p.x / 1e6, p.y / 1e6


def angle_error(a, b):
    angle = math.degrees(math.atan2(b[1]-a[1], b[0]-a[0])) % 45
    return min(angle, 45-angle)


def simple_paths(a, b):
    """Straight/one-bend octilinear alternatives, not a general autorouter."""
    dx, dy = b[0]-a[0], b[1]-a[1]
    if min(abs(dx), abs(dy)) < EPS or abs(abs(dx)-abs(dy)) < EPS:
        yield [a, b]
    sx = 1 if dx >= 0 else -1
    sy = 1 if dy >= 0 else -1
    length = min(abs(dx), abs(dy))
    yield [a, (a[0]+sx*length, a[1]+sy*length), b]
    yield [a, (b[0]-sx*length, b[1]-sy*length), b]
    yield [a, (a[0], b[1]), b]
    yield [a, (b[0], a[1]), b]


def scan(path):
    board = k.LoadBoard(str(path))
    tracks = [t for t in board.GetTracks() if not isinstance(t, k.PCB_VIA)]
    vias = [t for t in board.GetTracks() if isinstance(t, k.PCB_VIA)]
    pads = [(f.GetReference(), p) for f in board.GetFootprints() for p in f.Pads()]
    findings = []

    def finding(kind, severity, items, explanation, **extra):
        ids = sorted(i.m_Uuid.AsString() for i in items)
        key = json.dumps([kind, ids], separators=(',', ':'))
        first = items[0]
        findings.append(dict(id=hashlib.sha256(key.encode()).hexdigest()[:16],
            kind=kind, severity=severity, net=first.GetNetname(),
            layer=board.GetLayerName(first.GetLayer()),
            at_mm=xy(first.GetPosition()), items=ids,
            explanation=explanation, **extra))

    groups = defaultdict(list)
    junctions = defaultdict(list)
    shapes = defaultdict(list)
    for track in tracks:
        if isinstance(track, k.PCB_ARC):
            finding('curved_track', 'error', [track], 'The project uses straight track segments.')
            continue
        a, b = xy(track.GetStart()), xy(track.GetEnd())
        length = math.dist(a, b)
        if length < .00001:
            finding('zero_length', 'error', [track], 'Remove zero-length copper.')
            continue
        angle = angle_error(a, b)
        if angle > ANGLE_TOLERANCE_DEG:
            finding('off_angle', 'error', [track], 'Route at 0/45/90 degrees.',
                    deviation_deg=round(angle, 4), start_mm=a, end_mm=b)
        if track.GetLayer() in REFERENCE_LAYERS:
            finding('inner_track', 'error', [track], 'Inner layers are reserved for GND planes.')
        width = track.GetWidth()/1e6
        if length < SHORT_SEGMENT_WIDTHS*width:
            finding('short_segment', 'review', [track],
                    'Check for a corrective micro-jog; a centred pad escape can be legitimate.',
                    length_mm=round(length, 5), width_mm=width, start_mm=a, end_mm=b)
        group = track.GetNetname(), track.GetLayer()
        groups[group].append(track)
        for p in (a, b):
            junctions[group, tuple(round(v, 5) for v in p)].append(track)
        shapes[track.GetLayer()].append((track.GetNetname(), LineString([a, b]).buffer(width/2)))
    for via in vias:
        for layer in COPPER_LAYERS:
            shapes[layer].append((via.GetNetname(), Point(xy(via.GetPosition())).buffer(via.GetWidth(layer)/2e6)))
    for ref, pad in pads:
        for layer in COPPER_LAYERS:
            if pad.IsOnLayer(layer):
                shapes[layer].append((pad.GetNetname(), unary_union(polygon_set(pad.GetEffectivePolygon(layer)))))

    # A stitch/thermal via joins planes, even without an attached track. Count
    # those connections explicitly instead of declaring every trackless via bad.
    via_copper_cache = {}
    for via in vias:
        connected_layers = []
        centre = Point(xy(via.GetPosition()))
        for layer in COPPER_LAYERS:
            key = via.GetNetname(), layer
            if key not in via_copper_cache:
                copper = [LineString([xy(t.GetStart()), xy(t.GetEnd())]).buffer(t.GetWidth()/2e6)
                          for t in groups.get(key, [])]
                copper += [g for _, pad in pads if pad.GetNetname() == key[0] and pad.IsOnLayer(layer)
                           for g in polygon_set(pad.GetEffectivePolygon(layer))]
                copper += [g for zone in board.Zones()
                           if not zone.GetIsRuleArea() and zone.IsOnLayer(layer)
                           and zone.GetNetname() == key[0]
                           for g in polygon_set(zone.GetFilledPolysList(layer))]
                via_copper_cache[key] = unary_union(copper)
            if via_copper_cache[key].intersects(centre.buffer(via.GetWidth(layer)/2e6)):
                connected_layers.append(board.GetLayerName(layer))
        if len(connected_layers) < 2:
            finding('via_without_interlayer_role', 'review', [via],
                    'No two-layer connection found; check whether this via has a real electrical/thermal role.',
                    connected_layers=connected_layers)

    # Partial centreline overlap is distinct from harmless overlap at a corner.
    for group, items in groups.items():
        lines = [LineString([xy(t.GetStart()), xy(t.GetEnd())]) for t in items]
        tree = STRtree(lines)
        for i, line in enumerate(lines):
            for j in tree.query(line.buffer(EPS)):
                if j <= i:
                    continue
                overlap = line.intersection(lines[j]).length
                if overlap > EPS:
                    finding('collinear_overlap', 'review', [items[i], items[j]],
                        'Remove redundant overlap; retain only a justified width transition.',
                        overlap_mm=round(overlap, 5))
                if line.hausdorff_distance(lines[j]) < .00001 and items[i].GetWidth() == items[j].GetWidth():
                    finding('duplicate_track', 'error', [items[i], items[j]], 'Duplicate same-layer copper.')

    foreign_cache = {}
    for (group, point), items in junctions.items():
        if len(items) != 2:
            continue
        a, b = items
        other = [xy(t.GetEnd()) if math.dist(xy(t.GetStart()), point) < EPS else xy(t.GetStart()) for t in items]
        u, v = [(p[0]-point[0], p[1]-point[1]) for p in other]
        if min(math.hypot(*u), math.hypot(*v)) < EPS:
            continue
        cosine = sum(x*y for x,y in zip(u,v))/(math.hypot(*u)*math.hypot(*v))
        interior = math.degrees(math.acos(max(-1, min(1, cosine))))
        if interior < 89.9:
            finding('hairpin', 'review', items, 'Acute reversal: inspect detour and full-width junction.',
                    interior_angle_deg=round(interior, 3))
        anchored = any(p.GetNetname() == group[0] and p.IsOnLayer(group[1])
                       and p.GetEffectiveShape(group[1]).Collide(k.VECTOR2I(k.FromMM(point[0]), k.FromMM(point[1])))
                       for _, p in pads)
        anchored |= any(t.GetNetname() == group[0] and math.dist(xy(t.GetPosition()), point) < EPS for t in vias)
        if interior > 179.9 and a.GetWidth() == b.GetWidth() and not anchored:
            finding('mergeable_collinear', 'review', items, 'Merge straight runs unless this junction is an explicit anchor.')

    # Inspect short alternating bends as a sequence. Fine-pitch escapes remain
    # review candidates rather than errors; neither length nor bend count alone
    # decides whether a route is justified.
    sequences = set()
    for (group, point), items in junctions.items():
        if len(items) != 2:
            continue
        for first, middle in (items, items[::-1]):
            far = xy(middle.GetEnd()) if math.dist(xy(middle.GetStart()), point) < EPS else xy(middle.GetStart())
            neighbours = junctions.get((group, tuple(round(v, 5) for v in far)), [])
            if len(neighbours) != 2:
                continue
            last = next((t for t in neighbours if t.m_Uuid != middle.m_Uuid), None)
            if last is None or last.m_Uuid == first.m_Uuid:
                continue
            chain = [first, middle, last]
            key = tuple(sorted(t.m_Uuid.AsString() for t in chain))
            if key in sequences:
                continue
            sequences.add(key)
            vectors = [(t.GetEnd().x-t.GetStart().x, t.GetEnd().y-t.GetStart().y) for t in chain]
            parallel = abs(vectors[0][0]*vectors[2][1]-vectors[0][1]*vectors[2][0]) < 10
            turn = abs(vectors[0][0]*vectors[1][1]-vectors[0][1]*vectors[1][0]) > 10
            if parallel and turn and middle.GetLength() < max(.5e6, 3*middle.GetWidth()):
                finding('short_zigzag', 'review', chain,
                        'Short direction correction between parallel runs; inspect the entire endpoint-to-endpoint route.')

    # A track's centreline must actually reach its destination copper. This also
    # checks GND/power; their thermal/current exceptions do not excuse edge touches.
    for ref, pad in pads:
        for layer in COPPER_LAYERS:
            if not pad.IsOnLayer(layer):
                continue
            polygon = unary_union(polygon_set(pad.GetEffectivePolygon(layer)))
            for track in groups.get((pad.GetNetname(), layer), []):
                line = LineString([xy(track.GetStart()), xy(track.GetEnd())])
                if line.distance(polygon) < track.GetWidth()/2e6-EPS and not line.intersects(polygon):
                    entered = any(LineString([xy(t.GetStart()), xy(t.GetEnd())]).intersects(polygon)
                                  for t in groups.get((pad.GetNetname(), layer), []))
                    finding('pad_edge_touch', 'review' if entered else 'error', [track, pad],
                        'An adjacent track cap overlaps the pad: inspect the complete entry and any redundant stub.'
                        if entered else 'Copper touches pad but no same-net track centreline enters it.',
                        pad=ref+'.'+pad.GetNumber())

    # Flag two-segment detours only when a shorter simple alternative clears
    # every foreign pad/track/via. This is a candidate, not clearance certification:
    # zones, board edge and circuit-sensitive routing still require KiCad review.
    for (group, point), items in junctions.items():
        if len(items) != 2 or any(isinstance(t, k.PCB_ARC) for t in items):
            continue
        if items[0].GetWidth() != items[1].GetWidth():
            continue
        ends = [xy(t.GetEnd()) if math.dist(xy(t.GetStart()), point) < EPS else xy(t.GetStart()) for t in items]
        old_length = sum(math.dist(p, point) for p in ends)
        if old_length < 1:
            continue
        if group not in foreign_cache:
            foreign_cache[group] = unary_union([shape for net, shape in shapes[group[1]] if net != group[0]])
        foreign = foreign_cache[group]
        margin = .15 + items[0].GetWidth()/2e6
        for points in simple_paths(*ends):
            candidate = LineString(points)
            if candidate.length < old_length-.5 and candidate.distance(foreign) >= margin:
                finding('avoidable_detour', 'review', items,
                    'A shorter octilinear alternative clears foreign pads/tracks/vias; check edge/zones and return path.',
                    alternative_mm=points, saving_mm=round(old_length-candidate.length, 4))
                break

    # Bind electrical geometry review to the same regional dispositions. Bare
    # regression fixtures do not have the complete four-layer plane policy.
    if all(any(not z.GetIsRuleArea() and z.GetNetname()=='GND' and z.IsOnLayer(l)
               for z in board.Zones()) for l in COPPER_LAYERS):
        from review_layout import review
        references=review(path)
        by_id={t.m_Uuid.AsString():t for t in board.GetTracks()}
        for item in references['transitions_requiring_dense_fanout_review']:
            finding('return_stitch_distance','review',[by_id[item['uuid']]],
                    'Return connection exceeds the 2 mm project review target; inspect fanout and plane continuity.',
                    distance_mm=item['distance_mm'],return_at_mm=item['return_at_mm'])
        for item in references['reference_gap_candidates']:
            finding('reference_gap','review',[by_id[item['uuid']]],
                    'Inspect local reference void beyond the signal own plated launch.',
                    max_distance_to_reference_mm=item['max_distance_to_reference_mm'])

    return dict(source_sha256=sha256(path),
        result='FAIL' if any(f['severity']=='error' for f in findings) else 'REVIEW REQUIRED' if findings else 'GEOMETRY CHECKS PASS',
        counts=dict(tracks=len(tracks), vias=len(vias),
                    errors=sum(f['severity']=='error' for f in findings),
                    review=sum(f['severity']=='review' for f in findings)),
        findings=sorted(findings, key=lambda f:(f['severity'], f['net'], f['kind'], f['id'])),
        limitations='Not visual sign-off, signal-integrity proof or a substitute for fresh-fill KiCad DRC. All findings require resolution.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board', type=Path)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    report = scan(args.board)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(report['result'], report['counts'])
    raise SystemExit(bool(report['counts']['errors']))


if __name__ == '__main__':
    main()
