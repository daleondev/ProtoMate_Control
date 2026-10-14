#!/usr/bin/env python3
"""Import reviewed routing and apply manufacturing settings to the project PCB."""
import argparse
import re
import pcbnew as k
from design import ROOT
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def point(x,y):return k.VECTOR2I(k.FromMM(x),k.FromMM(y))

def finish(name,session):
    b=k.LoadBoard(str(ROOT/'build'/name/(name+'.kicad_pcb')))
    if session:assert k.ImportSpecctraSES(b,session)
    height=35 if name=='display-adapter' else 53.34
    b.GetDesignSettings().SetAuxOrigin(point(0,height))
    for f in b.GetFootprints():
        if f.GetReference().startswith(('TP','H','FID')):
            f.SetAttributes(f.GetAttributes()|k.FP_EXCLUDE_FROM_BOM|k.FP_EXCLUDE_FROM_POS_FILES)
    for t in b.GetTracks():
        if isinstance(t,k.PCB_VIA):
            t.SetFrontTentingMode(k.TENTING_MODE_TENTED);t.SetBackTentingMode(k.TENTING_MODE_TENTED)
    def text(s,x,y,size=.8,layer=k.F_SilkS):
        t=k.PCB_TEXT(b);t.SetText(s);t.SetPosition(point(x,y));t.SetTextSize(point(size,size));t.SetTextThickness(k.FromMM(.15));t.SetLayer(layer)
        if layer==k.B_SilkS:t.SetMirrored(True)
        b.Add(t)
    if name=='display-adapter':
        for s,x,y in [('1',48.5,8.7),('80',6.2,8.7),('+',4.2,28.9),('GND',10,29.4),('3V3',20,34.2),('G',22,34.2),('CLK',38,16.5),('G',40,16.5),('SDA',47,16.5),('SCL',50,19.5)]:text(s,x,y)
        text('J1 PIN 1',22,28,1,k.B_SilkS)
        text('SMARTWIN / A',27.5,15,1,k.B_SilkS)
    else:
        for s,x,y in [('80',112.5,5.8),('1',112.5,47.2),('3V3',101,51),('PWR',104,51),('G',107,51)]:text(s,x,y)
        text('GIGA DISPLAY / A',57,7,1,k.B_SilkS)
    b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones())
    path=ROOT/name/(name+'.kicad_pcb');k.SaveBoard(str(path),b)
    # Explicit symmetrical fabrication stack. PCBWay may use equivalent FR-4
    # laminates while retaining finished thickness, copper and layer order.
    thickness=1.0 if name=='display-adapter' else 1.6
    core=thickness-.14-.36
    stack=f'''(stackup
      (layer "F.SilkS" (type "Top Silk Screen") (color "White"))
      (layer "F.Paste" (type "Top Solder Paste"))
      (layer "F.Mask" (type "Top Solder Mask") (color "Green") (thickness 0.01))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "prepreg") (thickness 0.18) (material "FR4") (epsilon_r 4.3) (loss_tangent 0.02))
      (layer "In1.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 2" (type "core") (thickness {core:.3f}) (material "FR4") (epsilon_r 4.3) (loss_tangent 0.02))
      (layer "In2.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 3" (type "prepreg") (thickness 0.18) (material "FR4") (epsilon_r 4.3) (loss_tangent 0.02))
      (layer "B.Cu" (type "copper") (thickness 0.035))
      (layer "B.Mask" (type "Bottom Solder Mask") (color "Green") (thickness 0.01))
      (layer "B.Paste" (type "Bottom Solder Paste"))
      (layer "B.SilkS" (type "Bottom Silk Screen") (color "White"))
      (copper_finish "ENIG") (dielectric_constraints no))'''
    s=path.read_text();s=s.replace('(setup\n','(setup\n\t\t'+stack+'\n',1);path.write_text(s)
    print(path)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('board',choices=['display-adapter','giga-adapter']);ap.add_argument('--session');a=ap.parse_args();finish(a.board,a.session)
