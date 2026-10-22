#!/usr/bin/env python3
"""Local mechanical envelope models, in mm; no network-dependent library links.

Connector envelopes follow the retained manufacturer drawings. They simplify
contact springs/latches and are not manufacturer production-tooling models.
"""
import sys
sys.path.append('/usr/lib/freecad/lib')
import FreeCAD as A
import Part
from design import ROOT, components
DEST=ROOT/'libraries/3dmodels'
def box(x,y,z,w,d,h):return Part.makeBox(w,d,h,A.Vector(x,y,z))
def save(name,shapes):Part.makeCompound(shapes).exportStep(str(DEST/(name+'.step')))

# Bourns SF-1206SA-M p3 nominal body and 0.51mm end caps; drawing-based envelope.
save('Fuse_Bourns_SF-1206SA-M',[box(-1.6,-.8,0,.51,1.6,.85),
    box(-1.09,-.8,0,2.18,1.6,.85),box(1.09,-.8,0,.51,1.6,.85)])

# KiCad STEP coordinates have +Y up, opposite the footprint editor.
# FH28H: rear solder tails at y=0, insertion toward +Y, contact 1 on +X.
shell=box(-22.45,.55,0,44.9,6,2.55)
slot=box(-20.285,3.1,.55,40.57,3.6,.45)
shell=shell.cut(slot)
contacts=[box(19.75-i*.5-.1,-.5,0,.2,1.3,.15) for i in range(80)]
tabs=[box(x-1.1,4.95,0,2.2,1.6,.15) for x in [-22.175,22.175]]
save('FH28H-80S',[shell,*contacts,*tabs])

# DF23 plug: 14 x 3.1 x 1.2; mated board spacing is 1.5mm.
shell=box(-7,-1.55,0,14,3.1,1.2).cut(box(-6.35,-.85,.35,12.7,1.7,1.0))
contacts=[box(-6+i*.5-.095,y,0,.19,.9,.15) for i in range(25) for y in [-2.15,1.25]]
save('DF23C-50DP',[shell,*contacts])

# J10 uses the retained KiCad BarrelJack_Horizontal STEP model and the
# user-supplied DC-005 footprint; do not regenerate its terminal geometry.

# TI PWP0016A: nominal HTSSOP body/lead envelope and exposed thermal pad.
save('TPS26600-PWP0016A',[box(-2.2,-2.5,.1,4.4,5,1.1),box(-1.5,-1.5,0,3,3,.1),
    *[box(x,-2.275+i*.65-.15,0,1,.3,.15) for x in [-3.2,2.2] for i in range(8)]])

# TRACO TSR 2-2433: drawing-based envelope, body 7.5 x 14 x 10.1.
# Axis/pin datum follows the local KiCad footprint. Trim tails for the display.
save('TSR2-2433-trimmed',[box(-3.89,-9.58,0,7.5,14,10.1),
    *[box(-.25,-y-.25,-1.8,.5,.5,1.8) for y in [0,2.54,5.08]]])

# TSR 1-2450 uses the stock body/pin geometry, with display-side tails trimmed.
stock=Part.read('/usr/share/kicad/3dmodels/Converter_DCDC.3dshapes/Converter_DCDC_TRACO_TSR-1_THT.step')
stock=stock.common(box(-50,-50,-2.4,100,100,60))
stock.exportStep(str(DEST/'TSR1-2450-trimmed.step'))

for p in components('giga-adapter'):
    if p['kind']!='HEADER':continue
    xs=[v['x'] for v in p['pads']];ys=[-v['y'] for v in p['pads']]
    vertical=max(ys)-min(ys)>max(xs)-min(xs)
    rows=2 if p['ref'] in ['J8','J9'] else 1
    nx=len(set(xs));ny=len(set(ys))
    ex=1.205 if vertical else 1.525
    ey=1.525 if vertical else 1.205
    body=box(min(xs)-ex,min(ys)-ey,0,max(xs)-min(xs)+2*ex,max(ys)-min(ys)+2*ey,8.51)
    tails=[];tail=14.83 if p['ref']=='J9' else 10
    for v in p['pads']:
        x,y=v['x'],-v['y']
        body=body.cut(box(x-.4,y-.4,2,.8,.8,7))
        tails.append(box(x-.32,y-.32,-tail,.64,.64,tail+2))
    save(p['footprint'],[body,*tails])
print('Created connector/header envelope STEP models.')
