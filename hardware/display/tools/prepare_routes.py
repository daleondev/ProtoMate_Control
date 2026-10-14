#!/usr/bin/env python3
"""Power/return prerequisites before signal autorouting (KiCad pcbnew)."""
from pathlib import Path
import re
import pcbnew as k
from ground_fanout import add_ground_fanout
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
        track('+3V3_INPUT',1.2,[(6,24),(6,26),(8,28),(10.7125,28),(12.7125,26)])
        track('+3V3_FUSED',.8,[(14.2875,26),(16.9375,26),(16.9375,29)])
        track('+3V3_FUSED',.8,[(16.9375,29),(17.5375,28.4),(18.3,28.4)])
        track('+3V3_FUSED',.8,[(16.9375,33),(17.8,33),(18.3,32.5)])
        for y in [28.4,32.5]:via('+3V3_FUSED',18.3,y)
        track('+3V3_FUSED',.8,[(18.3,28.4),(18.3,32.5)],k.B_Cu)
        for y in [29.95,33.95]:
            track('+3V3_PANEL',.6,[(15.0625,y),(13.8,y)])
            via('+3V3_PANEL',13.8,y)
        track('+3V3_PANEL',.8,[(13.8,29.95),(13.8,33.95)],k.B_Cu)
        track('+3V3_PANEL',.8,[(15.0625,29.95),(18.0,29.95),(18.475,29.475),(20,29.475)])
        track('+3V3_PANEL',1.5,[(20,29.475),(32,29.475)])
        track('+3V3_PANEL',.8,[(30,29.475),(30,23)])
        track('+3V3_PANEL',1.2,[(30,23),(22.5,23)])
        for x in [22.5,23.3]:via('+3V3_PANEL',x,23)
        track('+3V3_PANEL',.5,[(22.5,23),(23.3,23)],k.B_Cu)
        # Short 0.25mm pad escapes; broad common rail on the connector's inside.
        track('+3V3_PANEL',.5,[(22.5,23),(22.5,21.85),(23.5,21.85)],k.B_Cu)
        track('+3V3_PANEL',.5,[(22.5,23),(22.5,24.15),(23,24.15)],k.B_Cu)
        for x in [22.5,23,23.5]:track('+3V3_PANEL',.25,[(x,21.85),(x,21.05)],k.B_Cu)
        for x in [22.5,23]:track('+3V3_PANEL',.25,[(x,24.15),(x,24.95)],k.B_Cu)
        # Low-impedance return: parallel vias by terminal, bulk caps and connector.
        for x,y in [(9.5,22.5),(10.3,22.5),(20,27.7),(24,27.7),(28,27.7),(32,27.7),(20.2,21.05),(20.2,24.95)]:
            via('GND',x,y)
        track('GND',1.2,[(9.5,24),(9.5,22.5),(10.3,22.5)])
        for x in [20,24,28,32]:track('GND',.8,[(x,26.525),(x,27.7)])
        for y in [21.05,24.95]:track('GND',.25,[(20.2,y),(21.5,y),(22,y)],k.B_Cu)
    else:
        # Keep the MCU supply off the dense signal fan-out. Four direct cap vias
        # share a back-side spine, with two continuous internal ground planes.
        for ys in [8.5,20,31.5,43]:
            y=ys-5.35
            track('+3V3_GIGA',.3,[(101.175,y),(100.6,y),(100.15,y-.45)])
            via('+3V3_GIGA',100.15,y-.45)
        track('+3V3_GIGA',.4,[(100.15,2.7),(100.15,37.2)],k.B_Cu)
    add_ground_fanout(b,which)
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
    dsn.write_text(s)

if __name__=='__main__':
    for which in ['display-adapter','giga-adapter']:prepare(which)
