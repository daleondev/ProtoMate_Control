"""Extract mating geometry from the official Altium board imported by KiCad."""
import json
import math
from pathlib import Path
import sys
from sexpr import child, children, parse, property_value

b = parse(Path(sys.argv[1]).read_text())
nets = {x[1]: x[2] for x in children(b, 'net')}
origin = (97.7011, 78.3336)
out = {'source': 'Arduino ABX00063 PCB.PcbDoc; KiCad 10.0.6 Altium import',
       'origin_mm': origin, 'headers': {}, 'mounting_holes': [], 'outline': []}
for f in children(b, 'footprint'):
    ref = property_value(f, 'Reference')
    at = child(f, 'at')
    x, y = map(float, at[1:3])
    ang = math.radians(float(at[3]) if len(at) > 3 else 0)
    if ref in ['JDIGITAL', 'JANALOG', 'JSIDE', 'J5', 'J6']:
        pads = []
        for p in children(f, 'pad'):
            if not p[1]:
                continue
            a = child(p, 'at')
            dx, dy = map(float, a[1:3])
            px = x + dx * math.cos(ang) + dy * math.sin(ang)
            py = y - dx * math.sin(ang) + dy * math.cos(ang)
            n = child(p, 'net', ['net', '0'])
            net = n[2] if len(n) > 2 else nets.get(n[1], n[1] if not n[1].isdigit() else '')
            if ref in ['J5', 'J6']:
                # The solder tails are 6.1 mm apart. Mating columns are 2.54 mm apart.
                px += 1.78 if int(p[1]) % 2 else -1.78
            pads.append({'pin': int(p[1]), 'x': round(px-origin[0], 5),
                         'y': round(py-origin[1], 5), 'net': net})
        out['headers'][ref] = pads
    if not ref:
        for p in children(f, 'pad'):
            d = child(p, 'drill')
            if d and abs(float(d[1])-3.2) < .01:
                out['mounting_holes'].append([round(x-origin[0], 5), round(y-origin[1], 5)])
for s in children(b, 'gr_line') + children(b, 'gr_arc'):
    if child(s, 'layer')[1] == 'Edge.Cuts' and child(child(s, 'stroke'), 'width')[1] == '0.05':
        tags = ['start', 'mid', 'end'] if s[0] == 'gr_arc' else ['start', 'end']
        out['outline'].append({'type': s[0], 'points': [
            [round(float(child(s,t)[1])-origin[0],5), round(float(child(s,t)[2])-origin[1],5)]
            for t in tags]})
Path(sys.argv[2]).write_text(json.dumps(out, indent=2) + '\n')
