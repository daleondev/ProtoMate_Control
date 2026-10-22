#!/usr/bin/env python3
"""Check routed power paths and the isolated eFuse thermal island after DRC.

These geometry/resistance checks do not qualify load transients or temperatures.
"""
import hashlib
import json
import pcbnew as k
from shapely.geometry import Point, Polygon
from shapely.ops import unary_union
from design import ROOT
from power_routes import resistance_path

k.SwigPyIterator.next = k.SwigPyIterator.__next__

def xy(p):
    return k.ToMM(p.x), k.ToMM(p.y)

def zone_copper(board, net, layer):
    polygons = []
    def contour(c):
        return [xy(c.CPoint(i)) for i in range(c.PointCount())]
    for zone in board.Zones():
        if zone.GetIsRuleArea() or zone.GetNetname() != net or zone.GetLayer() != layer:
            continue
        fill = zone.GetFilledPolysList(layer)
        for i in range(fill.OutlineCount()):
            polygons.append(Polygon(contour(fill.COutline(i)), [
                contour(fill.CHole(i, j)) for j in range(fill.HoleCount(i))]).buffer(0))
    return unary_union(polygons)

def main():
    paths = {name: ROOT/name/(name+'.kicad_pcb')
             for name in ['display-adapter', 'giga-adapter']}
    boards = {name: k.LoadBoard(str(path)) for name, path in paths.items()}
    g = boards['giga-adapter']
    # F1's manufacturer land pattern must not be conflated with F2's stock 1206.
    f1 = next(f for f in g.GetFootprints() if f.GetReference() == 'F1')
    assert f1.GetField('MPN').GetText() == 'SF-1206SA200M-2'
    assert str(f1.GetFPID().GetLibItemName()) == 'Fuse_Bourns_SF-1206SA-M'
    fuse_pads = sorted(f1.Pads(), key=lambda p: p.GetNumber())
    assert len(fuse_pads) == 2
    assert all(xy(p.GetSize()) == (1.45, 1.8) for p in fuse_pads)
    spacing = abs(k.ToMM(fuse_pads[1].GetPosition().x-fuse_pads[0].GetPosition().x))
    assert abs(spacing-2.95) < .000001
    routes = []
    # Actual copper paths, independently of the pad net labels.
    for name, start, end in [
        ('giga-adapter', ('J10', 1), ('F1', 1)),
        ('giga-adapter', ('F1', 2), ('U7', 1)),
        ('giga-adapter', ('U7', 16), ('U8', 1)),
        ('giga-adapter', ('U7', 16), ('F2', 1)),
        ('giga-adapter', ('U8', 3), ('D3', 2)),
        ('giga-adapter', ('D3', 1), ('J5', 8)),
        ('giga-adapter', ('D3', 1), ('J9', 24)),
        ('display-adapter', ('U1', 1), ('C9', 1)),
        ('display-adapter', ('U1', 1), ('C10', 1)),
        ('display-adapter', ('U1', 1), ('U2', 1)),
        ('display-adapter', ('U2', 3), ('U3', 9)),
        ('display-adapter', ('U3', 1), ('C13', 1)),
        ('display-adapter', ('U3', 1), ('C14', 1)),
        ('display-adapter', ('U3', 1), ('TP1', 1)),
        *[('display-adapter', ('U3', 1), ('J2', pin)) for pin in [72, 74, 76]],
    ]:
        resistance, _ = resistance_path(boards[name], *start, *end)
        routes.append(dict(board=name, start=f'{start[0]}.{start[1]}',
                           end=f'{end[0]}.{end[1]}', hot_copper_ohm=resistance))
    fused = [t for t in g.GetTracks() if not isinstance(t, k.PCB_VIA)
             and t.GetNetname() == '+24V_FUSED']
    assert fused and min(k.ToMM(t.GetWidth()) for t in fused) >= .6-1e-6
    thermal = [t for t in g.GetTracks() if isinstance(t, k.PCB_VIA)
               and t.GetNetname() == 'PWR_RTN']
    assert len(thermal) == 4
    thermal_area = {}
    for layer in [k.F_Cu, k.B_Cu]:
        copper = zone_copper(g, 'PWR_RTN', layer)
        assert not copper.is_empty
        assert all(copper.covers(Point(xy(v.GetPosition()))) for v in thermal)
        thermal_area[g.GetLayerName(layer)] = copper.area
    u7 = next(f for f in g.GetFootprints() if f.GetReference() == 'U7')
    exposed = next(p for p in u7.Pads() if p.GetNumber() == '17')
    assert exposed.GetNetname() == 'PWR_RTN'
    assert zone_copper(g, 'PWR_RTN', k.F_Cu).covers(Point(xy(exposed.GetPosition())))
    # Smartwin p12: BKL 5..8, TFT 10, touch 49.
    loads = json.loads((ROOT/'manufacturing/verification/power-routing.json').read_text())
    assert {l['pad'] for l in loads['loads'] if l['load'] == 'backlight'} == {
        'J1.5', 'J1.6', 'J1.7', 'J1.8'}
    assert next(l['pad'] for l in loads['loads'] if l['load'] == 'TFT') == 'J1.10'
    report = dict(result='PASS — routed power connectivity and geometry',
        source_sha256={name: hashlib.sha256(p.read_bytes()).hexdigest() for name,p in paths.items()},
        routes=routes, fused_input_minimum_track_width_mm=.6,
        input_fuse=dict(mpn='SF-1206SA200M-2',pad_size_mm=[1.45,1.8],
                        pad_centre_spacing_mm=spacing,gap_mm=1.5),
        RTN_thermal_vias=len(thermal), RTN_zone_area_mm2=thermal_area,
        backlight_contacts=[5,6,7,8], TFT_contact=10, touch_contact=49,
        ground_isolation='RTN is separate from GND; verified by fresh-fill DRC and schematic parity.',
        assumptions='35um copper, 1.3 hot-resistance factor; route estimates omit contact and fuse resistance.',
        physical_qualification='PENDING — load steps, startup, current sharing, thermal and fault tests')
    (ROOT/'manufacturing/verification/power-review.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Power route / RTN thermal checks PASS')

if __name__ == '__main__':main()
