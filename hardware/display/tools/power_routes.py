#!/usr/bin/env python3
"""Estimate supply drops from routed copper and explicit per-load current budgets."""
import csv,heapq,json,math
from collections import defaultdict
import pcbnew as k
from design import ROOT,REVISION
k.SwigPyIterator.next=k.SwigPyIterator.__next__
LAYERS=[k.F_Cu,k.In1_Cu,k.In2_Cu,k.B_Cu]
def coord(p):return round(k.ToMM(p.x),6),round(k.ToMM(p.y),6)
def resistance_path(b,ref1,pin1,ref2,pin2):
    fp={f.GetReference():f for f in b.GetFootprints()}
    a=next(p for p in fp[ref1].Pads() if p.GetNumber()==str(pin1));c=next(p for p in fp[ref2].Pads() if p.GetNumber()==str(pin2));net=a.GetNetname();assert c.GetNetname()==net
    tracks=[t for t in b.GetTracks() if t.GetNetname()==net];pads=[p for f in b.GetFootprints() for p in f.Pads() if p.GetNetname()==net]
    nodes=set();edges=defaultdict(list);lines=[];vias=[];ends=[]
    def add(u,v,d):edges[u].append((v,d));edges[v].append((u,d))
    for t in tracks:
        if isinstance(t,k.PCB_VIA):
            x,y=coord(t.GetPosition());ns=[(l,x,y) for l in LAYERS];nodes.update(ns);vias.append(ns)
        else:
            u=(t.GetLayer(),*coord(t.GetStart()));v=(t.GetLayer(),*coord(t.GetEnd()));nodes.update([u,v]);lines.append((u,v,k.ToMM(t.GetWidth())))
            ends.extend([(u,k.ToMM(t.GetWidth())/2),(v,k.ToMM(t.GetWidth())/2)])
    for p in pads:
        for l in LAYERS:
            if p.IsOnLayer(l):nodes.add((l,*coord(p.GetPosition())))
    # Include pad contacts in the middle of a merged track. Copper beneath a
    # pad does not need a separate segment/vertex to form an electrical joint.
    for p in pads:
        px,py=coord(p.GetPosition())
        for u,v,width in lines:
            l,x,y=u;_,xx,yy=v;dx,dy=xx-x,yy-y;dd=dx*dx+dy*dy
            if not dd or not p.IsOnLayer(l):continue
            fraction=max(0,min(1,((px-x)*dx+(py-y)*dy)/dd))
            q=(l,round(x+fraction*dx,6),round(y+fraction*dy,6))
            if p.GetEffectiveShape(l).Collide(k.VECTOR2I(k.FromMM(q[1]),k.FromMM(q[2])),k.FromMM(.002)):
                nodes.add(q)
    # Include physical contact between round trace ends. Use the overlap's
    # narrowest chord (or the narrower track), rather than a zero-cost join.
    for i,(u,radius) in enumerate(ends):
        for v,other_radius in ends[i+1:]:
            if u[0]!=v[0] or u==v:continue
            distance=math.hypot(u[1]-v[1],u[2]-v[2])
            if distance>=radius+other_radius:continue
            width=2*min(radius,other_radius)
            if distance>abs(radius-other_radius):
                chord_offset=(distance**2+radius**2-other_radius**2)/(2*distance)
                width=min(width,2*math.sqrt(max(0,radius**2-chord_offset**2)))
            if width>1e-6:add(u,v,distance*.0175*1.3/(1000*width*.035))
    # A branch may end inside a wide trace without reaching its centreline.
    # KiCad correctly connects such copper; include the projected junction in
    # the resistance graph rather than falsely reporting an open circuit.
    # Lateral travel is charged at the narrower conductor's width.
    for u,v,width in lines:
        layer,x,y=u;_,xx,yy=v;dx,dy=xx-x,yy-y;dd=dx*dx+dy*dy
        if dd<1e-12:continue
        for start,end,other_width in lines:
            if start[0]!=layer:continue
            for n in [start,end]:
                t=((n[1]-x)*dx+(n[2]-y)*dy)/dd
                if not 0<=t<=1:continue
                projection=(layer,round(x+t*dx,6),round(y+t*dy,6))
                distance=math.hypot(n[1]-projection[1],n[2]-projection[2])
                if .002<distance<=width/2:
                    nodes.add(projection)
                    add(n,projection,distance*.0175*1.3/(1000*min(width,other_width)*.035))
    for u,v,width in lines:
        l,x,y=u;_,xx,yy=v;dx,dy=xx-x,yy-y;dd=dx*dx+dy*dy
        if dd<1e-12:continue
        points=[]
        for n in nodes:
            if n[0]!=l:continue
            t=((n[1]-x)*dx+(n[2]-y)*dy)/dd
            if -.00001<=t<=1.00001 and abs((n[1]-x)*dy-(n[2]-y)*dx)/math.sqrt(dd)<.002:points.append((t,n))
        points.sort()
        for (_,s),(_,e) in zip(points,points[1:]):add(s,e,math.hypot(e[1]-s[1],e[2]-s[2]) * .0175 * 1.3 / (1000 * width * .035))
    for p in pads:
        x,y=coord(p.GetPosition());ns=[]
        for l in LAYERS:
            if not p.IsOnLayer(l):continue
            centre=(l,x,y);ns.append(centre);shape=p.GetEffectiveShape(l)
            for n in nodes:
                if n[0]==l and shape.Collide(k.VECTOR2I(k.FromMM(n[1]),k.FromMM(n[2])),k.FromMM(.002)):add(centre,n,0)
        if p.GetDrillSize().x:vias.append(ns)
    for ns in vias:
        for u in ns:
            for v in ns:
                if u!=v:add(u,v,.002 if not any(p.GetDrillSize().x and coord(p.GetPosition())==u[1:] for p in pads) else 0)
    sources=[(l,*coord(a.GetPosition())) for l in LAYERS if a.IsOnLayer(l)];targets={(l,*coord(c.GetPosition())) for l in LAYERS if c.IsOnLayer(l)}
    q=[(0,n) for n in sources];heapq.heapify(q)
    cost={n:0 for n in sources};previous={}
    while q:
        d,n=heapq.heappop(q)
        if d!=cost.get(n):continue
        if n in targets:
            path=[]
            while n in previous:
                p,w=previous[n];path.append((tuple(sorted((p,n))),w));n=p
            return d,path
        for v,w in edges[n]:
            nd=d+w
            if nd<cost.get(v,float('inf')):cost[v]=nd;previous[v]=(n,w);heapq.heappush(q,(nd,v))
    raise RuntimeError(f'No measured route {ref1}.{pin1} -> {ref2}.{pin2} ({net})')


def main():
    b=k.LoadBoard(str(ROOT/'display-adapter/display-adapter.kicad_pcb'))
    # Explicit budgeting split; these currents are NOT guaranteed panel maxima.
    # Smartwin interface table, p12: BKL = pins 5..8; TFT = pin 10.
    loads=[('J1',pin,1.05/4,'backlight') for pin in [5,6,7,8]]
    loads += [('J1',10,.25,'TFT'),('J1',49,.05,'touch'),('J2',56,.15,'interface reference rail')]
    paths=[];currents=defaultdict(float)
    for ref,pin,amps,label in loads:
        resistance,path=resistance_path(b,'U1' if label=='backlight' else 'U3',3 if label=='backlight' else 1,ref,pin)
        paths.append((ref,pin,amps,label,resistance,path))
        for edge,r in path:currents[edge]+=amps
    results=[]
    for ref,pin,amps,label,resistance,path in paths:
        drop=sum(r*currents[edge] for edge,r in path)
        results.append(dict(load=label,pad=f'{ref}.{pin}',budget_A=amps,selected_path_ohm=resistance,
                            supply_drop_V=drop))
    result=dict(revision=REVISION,method='Measured connected copper paths with length/width resistance at 35um; 1.3 hot-copper factor. Shared segments carry the sum of allocated loads. Parallel paths and copper pours are ignored. Pad interiors treated as equipotential; each via transition allocated 2mOhm. Return copper and connector losses remain separate budget allowances.',
                total_budget_A=sum(p[2] for p in paths),loads=results,
                maximum_logic_supply_drop_V=max(r['supply_drop_V'] for r in results if r['load']!='backlight'),
                maximum_backlight_supply_drop_V=max(r['supply_drop_V'] for r in results if r['load']=='backlight'),
                maximum_panel_supply_drop_V=max(r['supply_drop_V'] for r in results if r['pad'].startswith('J1')),
                return_allowance_ohm=.02,physical_verification='PENDING: actual current, via plating, return spreading and temperature')
    (ROOT/'manufacturing/verification/power-routing.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
