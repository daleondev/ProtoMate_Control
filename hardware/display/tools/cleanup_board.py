#!/usr/bin/env python3
"""Finalize annotations without changing copper or component placement."""
import json,re,sys
import pcbnew as k
from design import ROOT, BOARDS
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def main(name):
    path=ROOT/name/(name+'.kicad_pcb');parts=json.loads((ROOT/name/'components.json').read_text())
    refs={p['ref'] for p in parts if p['kind'] not in ['HEADER','FFC','DF23','POWER','TP']}
    s=path.read_text()
    def clean(m):
        ref=re.search(r'\(gr_text "([^"]+)"',m[0]);return '' if ref and ref[1] in refs else m[0]
    s=re.sub(r'(?ms)^\t\(gr_text .*?^\t\)\n',clean,s);path.write_text(s)
    b=k.LoadBoard(str(path));tb=b.GetTitleBlock();tb.SetTitle(name+' / Smartwin–GIGA');tb.SetRevision(BOARDS[name]['revision']);tb.SetDate(BOARDS[name]['date']);tb.SetCompany('ProtoMate');b.SetTitleBlock(tb)
    if name=='giga-adapter':
        junction=k.VECTOR2I(k.FromMM(100.15),k.FromMM(37.2))
        for t in b.GetTracks():
            if isinstance(t,k.PCB_VIA) and t.m_Uuid.AsString()=='abd9cba6-62d4-48f1-a5bf-9ec0243d8cf8':
                t.SetPosition(junction)
            elif not isinstance(t,k.PCB_VIA):
                if abs(t.GetStart().x-junction.x)<=2 and abs(t.GetStart().y-junction.y)<=2:t.SetStart(junction)
                if abs(t.GetEnd().x-junction.x)<=2 and abs(t.GetEnd().y-junction.y)<=2:t.SetEnd(junction)
    for t in b.GetDrawings():
        if isinstance(t,k.PCB_TEXT) and t.GetText()=='CN1 PIN 1':t.SetText('J1 PIN 1')
        if isinstance(t,k.PCB_TEXT) and t.GetLayer() in (k.F_SilkS,k.B_SilkS):
            t.SetTextThickness(max(t.GetTextThickness(),k.FromMM(.15)))
        if isinstance(t,k.PCB_TEXT) and name=='giga-adapter' and t.GetText() in ('1','80'):
            t.SetPosition(k.VECTOR2I(k.FromMM(112.5),k.FromMM(47.2 if t.GetText()=='1' else 5.8)))
        if isinstance(t,k.PCB_TEXT) and name=='display-adapter' and t.GetText()=='SCL':t.SetPosition(k.VECTOR2I(k.FromMM(50),k.FromMM(19.5)))
        if isinstance(t,k.PCB_TEXT) and name=='display-adapter' and t.GetText()=='3V3 ONLY':t.SetPosition(k.VECTOR2I(k.FromMM(7),k.FromMM(19.2)))
    b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones());k.SaveBoard(str(path),b)
if __name__=='__main__':main(sys.argv[1])
