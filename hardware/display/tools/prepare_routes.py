#!/usr/bin/env python3
"""Power/return prerequisites before signal autorouting (KiCad pcbnew)."""
from pathlib import Path
import re
import pcbnew as k
from ground_fanout import add_ground_fanout
from prepare_display import prepare_display
from design import ROOT
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def xy(x,y):return k.VECTOR2I(k.FromMM(x),k.FromMM(y))

def prepare(which):
    path=ROOT/'build'/which/(which+'.kicad_pcb')
    b=k.LoadBoard(str(path)); nets={n.GetNetname():n for n in b.GetNetsByNetcode().values()}
    for t in list(b.GetTracks()):b.Remove(t)
    def track(net,width,points,layer=k.F_Cu):
        for a,c in zip(points,points[1:]):
            t=k.PCB_TRACK(b);t.SetStart(xy(*a));t.SetEnd(xy(*c));t.SetWidth(k.FromMM(width));t.SetLayer(layer);t.SetNet(nets[net]);t.SetLocked(True);b.Add(t)
    def via(net,x,y):
        t=k.PCB_VIA(b);t.SetPosition(xy(x,y));t.SetWidth(k.FromMM(.65));t.SetDrill(k.FromMM(.3));t.SetViaType(k.VIATYPE_THROUGH);t.SetLayerPair(k.F_Cu,k.B_Cu);t.SetNet(nets[net]);t.SetLocked(True);b.Add(t)
    if which=='display-adapter':
        prepare_display(b)
    else:
        # Keep the MCU supply off the dense signal fan-out. Four direct cap vias
        # share a back-side spine, referenced to the internal In1 ground plane.
        for ys in [8.5,20,31.5,43]:
            y=ys-5.35
            track('+3V3_GIGA',.3,[(101.175,y),(100.6,y),(100.15,y-.45)])
            via('+3V3_GIGA',100.15,y-.45)
        track('+3V3_GIGA',.4,[(100.15,2.7),(100.15,37.2)],k.B_Cu)
    if which=='giga-adapter':add_ground_fanout(b,which)
    b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones());k.SaveBoard(str(path),b)
    k.ExportSpecctraDSN(b,'/tmp/'+which+'.dsn')
    dsn=Path('/tmp/'+which+'.dsn');s=dsn.read_text()
    s=s.replace('    (boundary\n','''
    (autoroute_settings
      (autoroute on) (postroute on) (vias on)
      (layer_rule F.Cu (active on) (preferred_direction horizontal))
      (layer_rule In1.Cu (active off) (preferred_direction horizontal))
      (layer_rule In2.Cu (active on) (preferred_direction horizontal))
      (layer_rule B.Cu (active on) (preferred_direction vertical)))
    (boundary
''',1)
    if b.GetCopperLayerCount()==2:
        s=re.sub(r"^.*\(layer_rule In[12]\.Cu.*\n", "", s, flags=re.M)
    dsn.write_text(s)

if __name__=='__main__':
    for which in ['display-adapter','giga-adapter']:prepare(which)
