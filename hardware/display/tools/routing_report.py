#!/usr/bin/env python3
"""Measure electrically connected route lengths between specified physical pads."""
import csv,heapq,json,math
from collections import defaultdict
import pcbnew as k
from design import ROOT,SIGNALS
k.SwigPyIterator.next=k.SwigPyIterator.__next__
LAYERS=[k.F_Cu,k.In1_Cu,k.In2_Cu,k.B_Cu]
def coord(p):return round(k.ToMM(p.x),6),round(k.ToMM(p.y),6)
def length(b,ref1,pin1,ref2,pin2):
    fp={f.GetReference():f for f in b.GetFootprints()}
    a=next(p for p in fp[ref1].Pads() if p.GetNumber()==str(pin1));c=next(p for p in fp[ref2].Pads() if p.GetNumber()==str(pin2));net=a.GetNetname();assert c.GetNetname()==net
    tracks=[t for t in b.GetTracks() if t.GetNetname()==net];pads=[p for f in b.GetFootprints() for p in f.Pads() if p.GetNetname()==net]
    nodes=set();edges=defaultdict(list);lines=[];vias=[];ends=[]
    def add(u,v,d):edges[u].append((v,d));edges[v].append((u,d))
    for t in tracks:
        if isinstance(t,k.PCB_VIA):
            x,y=coord(t.GetPosition());ns=[(l,x,y) for l in LAYERS];nodes.update(ns);vias.append(ns)
        else:
            u=(t.GetLayer(),*coord(t.GetStart()));v=(t.GetLayer(),*coord(t.GetEnd()));nodes.update([u,v]);lines.append((u,v))
            ends.extend([(u,k.ToMM(t.GetWidth())/2),(v,k.ToMM(t.GetWidth())/2)])
    for p in pads:
        for l in LAYERS:
            if p.IsOnLayer(l):nodes.add((l,*coord(p.GetPosition())))
    # A merged track can cross a pad without ending inside it. Project the
    # pad centre onto the track and split there, rather than requiring a
    # redundant segment beneath the pad merely to satisfy this measurement.
    for p in pads:
        px,py=coord(p.GetPosition())
        for u,v in lines:
            l,x,y=u;_,xx,yy=v;dx,dy=xx-x,yy-y;dd=dx*dx+dy*dy
            if not dd or not p.IsOnLayer(l):continue
            fraction=max(0,min(1,((px-x)*dx+(py-y)*dy)/dd))
            q=(l,round(x+fraction*dx,6),round(y+fraction*dy,6))
            if p.GetEffectiveShape(l).Collide(k.VECTOR2I(k.FromMM(q[1]),k.FromMM(q[2])),k.FromMM(.002)):
                nodes.add(q)
    # Round trace ends can overlap without coincident centreline vertices.
    # Removing copper-covered helper segments must not make the measurement
    # graph report a false open. Charge the actual distance across the overlap.
    for i,(u,radius) in enumerate(ends):
        for v,other_radius in ends[i+1:]:
            if u[0]!=v[0] or u==v:continue
            distance=math.hypot(u[1]-v[1],u[2]-v[2])
            if distance<radius+other_radius:add(u,v,distance)
    for u,v in lines:
        l,x,y=u;_,xx,yy=v;dx,dy=xx-x,yy-y;dd=dx*dx+dy*dy
        if dd<1e-12:continue
        points=[]
        for n in nodes:
            if n[0]!=l:continue
            t=((n[1]-x)*dx+(n[2]-y)*dy)/dd
            if -.00001<=t<=1.00001 and abs((n[1]-x)*dy-(n[2]-y)*dx)/math.sqrt(dd)<.002:points.append((t,n))
        points.sort()
        for (_,s),(_,e) in zip(points,points[1:]):add(s,e,math.hypot(e[1]-s[1],e[2]-s[2]))
    for p in pads:
        x,y=coord(p.GetPosition());ns=[]
        for l in LAYERS:
            if not p.IsOnLayer(l):continue
            centre=(l,x,y);ns.append(centre);shape=p.GetEffectiveShape(l)
            for n in nodes:
                if n[0]==l and shape.Collide(k.VECTOR2I(k.FromMM(n[1]),k.FromMM(n[2])),k.FromMM(.002)):add(centre,n,math.hypot(n[1]-x,n[2]-y))
        if p.GetDrillSize().x:vias.append(ns)
    for ns in vias:
        for u in ns:
            for v in ns:
                if u!=v:add(u,v,0)
    sources=[(l,*coord(a.GetPosition())) for l in LAYERS if a.IsOnLayer(l)];targets={(l,*coord(c.GetPosition())) for l in LAYERS if c.IsOnLayer(l)}
    q=[(0,n) for n in sources];heapq.heapify(q)
    cost={n:0 for n in sources}
    while q:
        d,n=heapq.heappop(q)
        if d!=cost.get(n):continue
        if n in targets:return d
        for v,w in edges[n]:
            nd=d+w
            if nd<cost.get(v,float('inf')):cost[v]=nd;heapq.heappush(q,(nd,v))
    raise RuntimeError(f'No measured route {ref1}.{pin1} -> {ref2}.{pin2} ({net})')

def main():
    g=k.LoadBoard(str(ROOT/'giga-adapter/giga-adapter.kicad_pcb'));d=k.LoadBoard(str(ROOT/'display-adapter/display-adapter.kicad_pcb'))
    rows=[]
    for i,s in enumerate(SIGNALS[:28]):
        u=f'U{i//8+1}';ch=i%8
        row={'signal':s['name'],'giga_input_mm':length(g,s['adapter_header'],s['adapter_header_pin'],u,3+ch),'buffer_to_series_mm':length(g,u,21-ch,f'R{i+1}',1),'series_to_ffc_mm':length(g,f'R{i+1}',2,'J1',s['giga_ffc']),'display_ffc_to_panel_mm':length(d,'J2',s['display_ffc'],'J1',s['panel'])}
        row['total_adapter_copper_mm']=sum(v for v in row.values() if isinstance(v,float));rows.append(row)
    clock=next(r['total_adapter_copper_mm'] for r in rows if r['signal']=='PCLK')
    for r in rows:r['relative_to_clock_mm']=r['total_adapter_copper_mm']-clock
    report=ROOT/'manufacturing/verification'
    with (report/'route-lengths.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator="\n");w.writeheader();w.writerows([{a:round(b,4) if isinstance(b,float) else b for a,b in r.items()} for r in rows])
    timed_rows=[r for r in rows if r['signal'] in {s['name'] for s in SIGNALS[:24]}|{'DE','HSYNC','VSYNC'}]
    skew=max(abs(r['relative_to_clock_mm']) for r in timed_rows)*.007
    delta=[r['relative_to_clock_mm']*.007 for r in timed_rows]
    result={'method':'Shortest physical copper path, splitting segments at intersections and pad/via anchors. Horizontal lengths; package/via internal delay excluded.','pixel_clock_MHz':40.96,'half_period_ns':12.20703125,'panel_setup_hold_ns':5,'buffer_interdevice_worst_case_range_ns':3.5,'adapter_trace_skew_estimate_ns_at_7ps_per_mm':skew,'stm32_DS12930_table105_conditions':'VDD >2.7V, OSPEED=11, IO compensation enabled, stated load and voltage-scale conditions; Very-high GPIO speed and CSI-referenced IO compensation are configured in firmware; physical loading remains to be qualified.','stm32_data_valid_max_ns':.5,'stm32_data_hold_min_ns':0,'stm32_half_cycle_tolerance_ns':.5,'conditional_setup_margin_ns_before_unmodelled_terms':12.20703125-.5-.5-3.5-5-max(delta),'conditional_hold_margin_ns_before_unmodelled_terms':12.20703125-.5-3.5-5+min(delta),'unmodelled_terms':['Arduino PCB routes','FFC conductor skew, material and connector/package effects','Via discontinuities, rise/fall settling and clock jitter','Actual display sampling phase and compliance with ST characterization conditions'],'physical_timing_verification':'NOT PERFORMED. Calculated margin does not replace connector-level setup/hold measurement; qualify at the panel connector before accepting operation.'}
    result['timed_signals']=[r['signal'] for r in timed_rows]
    result['translator_characterization']={
        'source':'SN74AXC8T245 SCES875C section 5.13 / figures 6-2 and 6-3',
        'propagation_delay_min_max_ns':[.5,4], 'load_pF':15,
        'input_slew_ns_per_V_max':1,
        'measurement_voltage_fraction':.5,
        'qualification':'Actual cable/panel load is not measured. The 3.5ns spread is conditional on the datasheet test conditions, and panel thresholds are 30/70%, not 50%.'}
    result['timing_closure']='OPEN — conditional estimate is not a guaranteed interface timing result'
    result['additional_qualification_requirements']=[
        'Bootstrap enables CSI and waits for IO compensation READY before starting either application.',
        'Validate total buffer load and input slew against the TI switching-characteristic test conditions.',
        'Measure PCLK high/low times and setup/hold at the panel including translator pulse-width distortion.',
    ]
    result['display_timing']={'horizontal_total':1120,'vertical_total':610,'frame_rate_Hz':40.96e6/(1120*610),'status':'CONFIGURED in CubeMX and bootstrap; panel testing pending'}
    # First-order outer-layer estimate, not a controlled-impedance guarantee.
    # F/B are each 0.14 mm from their adjacent, continuous ground plane.
    # Nearby coplanar fill, mask and fabrication tolerances need a field solver
    # or actual measurement; do not hide them in a precise-looking single value.
    widths=sorted({round(t.GetWidth()/1e6,5) for board in (g,d)
                   for t in board.GetTracks() if not isinstance(t,k.PCB_VIA)
                   and t.GetNetname().startswith(('LCD_','MCU_','BUF_'))})
    def impedance(width,er):
        return 87/math.sqrt(er+1.41)*math.log(5.98*.14/(.8*width+.035))
    result['outer_layer_impedance_estimate']={
        'source':'https://www.ti.com/lit/an/sprabi2d/sprabi2d.pdf',
        'formula':'87/sqrt(er+1.41) * ln(5.98*h/(0.8*w+t))',
        'h_mm':.14,'copper_mm':.035,'assumed_er_range':[4.0,4.6],
        'widths':[dict(width_mm=w,estimate_ohm=[round(impedance(w,4.6),1),round(impedance(w,4.0),1)])
                  for w in widths if .1<=w<=.3],
        'limitations':'Microstrip approximation; outer GND fill changes this to locally coplanar geometry. Mask, actual laminate Dk, etching, vias, cable and headers are not modeled. No impedance-controlled fabrication claim.',
        'termination':'Keep the existing 33-ohm source resistors beside the buffers. Driver output resistance contributes to source impedance; qualify/tune the populated value from panel-end ringing and setup/hold measurements. No decorative delay meanders.'}
    (report/'timing-budget.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
