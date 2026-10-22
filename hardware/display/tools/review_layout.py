#!/usr/bin/env python3
"""Measure both boards' reference planes and high-speed return connections.

The 2 mm stitch distance is a project review target, not an IPC/AISLER limit.
Dense fanouts require individual inspection. This is geometry, not SI sign-off.
"""
import hashlib
import json
import math
from pathlib import Path

import pcbnew as k
from shapely.geometry import Point, LineString
from shapely.ops import unary_union

from audit_copper import polygon_set, xy
from design import ROOT, BOARDS

k.SwigPyIterator.next = k.SwigPyIterator.__next__
REFERENCE = {k.F_Cu: k.In1_Cu, k.B_Cu: k.In2_Cu}


def fast_signal(net):
    stem = net.removeprefix('LCD_').removeprefix('MCU_').removeprefix('BUF_')
    return stem in {'PCLK', 'DE', 'HSYNC', 'VSYNC'} or (
        len(stem) == 2 and stem[0] in 'RGB' and stem[1] in '01234567')


def review(path):
    b = k.LoadBoard(str(path))
    planes = {l: unary_union([p for z in b.Zones()
              if not z.GetIsRuleArea() and z.GetNetname() == 'GND' and z.IsOnLayer(l)
              for p in polygon_set(z.GetFilledPolysList(l))])
              for l in (k.F_Cu, k.In1_Cu, k.In2_Cu, k.B_Cu)}
    assert all(planes[l].geom_type == 'Polygon' for l in REFERENCE.values()), 'Split reference plane'
    vias = [v for v in b.GetTracks() if isinstance(v, k.PCB_VIA)]
    pads = [p for f in b.GetFootprints() for p in f.Pads()]
    grounds = [xy(v.GetPosition()) for v in vias if v.GetNetname() == 'GND']
    grounds += [xy(p.GetPosition()) for p in pads if p.GetNetname() == 'GND' and p.GetDrillSize().x]
    assert grounds
    transitions = []
    for v in vias:
        if not fast_signal(v.GetNetname()):
            continue
        q = xy(v.GetPosition())
        nearest = min(grounds, key=lambda p: math.dist(q, p))
        transitions.append(dict(uuid=v.m_Uuid.AsString(), net=v.GetNetname(), at_mm=q, return_at_mm=nearest,
                                distance_mm=round(math.dist(q, nearest), 4)))
    gaps = []
    coverage = []
    for t in b.GetTracks():
        if isinstance(t, k.PCB_VIA) or not fast_signal(t.GetNetname()):
            continue
        reference = REFERENCE[t.GetLayer()]
        line = LineString([xy(t.GetStart()), xy(t.GetEnd())])
        # A signal's own plated pad/via necessarily has a ground antipad.
        # Exclude that local launch only; foreign voids remain visible.
        launches = [Point(xy(p.GetPosition())).buffer(max(xy(p.GetSize()))/2 + .16)
                    for p in pads if p.GetNetname() == t.GetNetname() and p.GetDrillSize().x]
        launches += [Point(xy(v.GetPosition())).buffer(v.GetWidth(reference)/2e6 + .16)
                     for v in vias if v.GetNetname() == t.GetNetname()]
        permitted = unary_union(launches)
        n = max(1, math.ceil(line.length/.05))
        samples = [line.interpolate(i/n, normalized=True) for i in range(n+1)]
        outside = [q.distance(planes[reference]) for q in samples if not permitted.covers(q)]
        maximum = max(outside, default=0)
        coverage.append(maximum)
        if maximum > .05:
            gaps.append(dict(uuid=t.m_Uuid.AsString(), net=t.GetNetname(), layer=b.GetLayerName(t.GetLayer()),
                             start_mm=xy(t.GetStart()), end_mm=xy(t.GetEnd()),
                             max_distance_to_reference_mm=round(maximum, 4)))
    return dict(source_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
        plane_area_mm2={b.GetLayerName(l):round(p.area,2) for l,p in planes.items()},
        reference_layers={b.GetLayerName(a):b.GetLayerName(c) for a,c in REFERENCE.items()},
        continuous_inner_planes=True, ground_connections=len(grounds),
        transitions=transitions,
        maximum_transition_return_distance_mm=max(t['distance_mm'] for t in transitions),
        transition_review_target_mm=2,
        transitions_requiring_dense_fanout_review=[t for t in transitions if t['distance_mm']>2],
        maximum_nonlaunch_reference_gap_mm=round(max(coverage, default=0),4),
        reference_gap_candidates=gaps, sample_spacing_mm=.05,
        qualification='Geometry only. Distance targets identify inspection candidates; they are not manufacturing waivers or a measured signal-integrity guarantee.')


def main():
    for name in BOARDS:
        result=review(ROOT/name/(name+'.kicad_pcb'))
        (ROOT/'manufacturing/verification'/(name+'-layout-review.json')).write_text(json.dumps(result,indent=2)+'\n')
        print(name, 'continuous inner planes;', result['maximum_transition_return_distance_mm'],
              'mm maximum return distance;', len(result['reference_gap_candidates']), 'reference-gap candidates')


if __name__ == '__main__':
    main()
