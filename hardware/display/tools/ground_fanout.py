"""Place short ground escapes using KiCad's actual copper collision geometry."""
import math
import pcbnew as k
k.SwigPyIterator.next=k.SwigPyIterator.__next__
def mm(x):return k.FromMM(x)
def pt(x,y):return k.VECTOR2I(mm(x),mm(y))

def add_ground_fanout(b,which):
    pads=[p for f in b.GetFootprints() for p in f.Pads()]
    ground=next(n for n in b.GetNetsByNetcode().values() if n.GetNetname()=='GND')
    obstacles=[];holes=[];padshapes=[]
    for p in pads:
        for layer in (k.F_Cu,k.B_Cu):
            if p.IsOnLayer(layer):padshapes.append(p.GetEffectiveShape(layer))
        if p.GetDrillSize().x:
            holes.append(p.GetEffectiveHoleShape())
        if p.GetNetname()!='GND':
            for layer in (k.F_Cu,k.B_Cu):
                if p.IsOnLayer(layer):obstacles.append((layer,p.GetEffectiveShape(layer)))
    tracks=list(b.GetTracks())
    for t in tracks:
        if isinstance(t,k.PCB_VIA):
            holes.append(k.SHAPE_CIRCLE(t.GetPosition(),t.GetDrillValue()//2))
            if t.GetNetname()!='GND':
                for l in (k.F_Cu,k.B_Cu):obstacles.append((l,t.GetEffectiveShape(l)))
        elif t.GetNetname()!='GND':obstacles.append((t.GetLayer(),t.GetEffectiveShape()))
    def inside(x,y):
        if which=='display-adapter':return .65<=x<=54.35 and .65<=y<=34.35
        return .65<=y<=52.69 and 11.65<=x<=121.35 and (y<=9.35 and x>=12.65 or y>=47.65 or x>=78.65)
    completed=set(); connected_points=[]
    # Narrow connector contacts first, before the much less constrained passives.
    groundpads=sorted((p for p in pads if p.GetNetname()=='GND' and not p.GetDrillSize().x),key=lambda p:p.GetSize().x*p.GetSize().y)
    for pad in groundpads*3:
        identity=(pad.GetParentFootprint().GetReference(),pad.GetNumber())
        if identity in completed:continue
        layer=k.B_Cu if pad.IsOnLayer(k.B_Cu) else k.F_Cu
        origin=pad.GetPosition();x,y=k.ToMM(origin.x),k.ToMM(origin.y)
        # Existing ground vias touching this pad/escape need no additional hole.
        existing=[t for t in b.GetTracks() if isinstance(t,k.PCB_VIA) and t.GetNetname()=='GND']
        candidates=[]
        for connected_layer,v in connected_points:
            if connected_layer!=layer:continue
            d=math.hypot(v.x-origin.x,v.y-origin.y)/1e6
            if .01<d<4.0:candidates.append((d,True,v))
        for t in existing:
            v=t.GetPosition();d=math.hypot(v.x-origin.x,v.y-origin.y)/1e6
            if d<4.0:candidates.append((d,True,v))
        for r in [.8,1.,1.2,1.4,1.6,1.8,2.,2.4,2.8,3.2]:
            for angle in [0,90,180,270,45,135,225,315,22.5,67.5,112.5,157.5,202.5,247.5,292.5,337.5]:
                a=math.radians(angle);v=pt(round(x+r*math.cos(a),4),round(y+r*math.sin(a),4))
                candidates.append((r,False,v))
        for _,old,v in sorted(candidates,key=lambda v:(v[0],not v[1])):
            vx,vy=k.ToMM(v.x),k.ToMM(v.y)
            if not inside(vx,vy):continue
            circle=k.SHAPE_CIRCLE(v,mm(.325));hole=k.SHAPE_CIRCLE(v,mm(.15))
            line=k.SHAPE_SEGMENT(origin,v,mm(.18))
            if any(k.SHAPE.Collide(line,s,mm(.15)) for l,s in obstacles if l==layer):continue
            if not old:
                # No via-in-pad, including pads on the same ground net.
                if any(k.SHAPE.Collide(circle,s,mm(.15)) for s in padshapes):continue
                if any(k.SHAPE.Collide(circle,s,mm(.15)) for l,s in obstacles):continue
                if any(k.SHAPE.Collide(hole,h,mm(.25)) for h in holes):continue
            t=k.PCB_TRACK(b);t.SetStart(origin);t.SetEnd(v);t.SetLayer(layer);t.SetWidth(mm(.18));t.SetNet(ground);t.SetLocked(True);b.Add(t)
            if not old:
                t=k.PCB_VIA(b);t.SetPosition(v);t.SetWidth(mm(.65));t.SetDrill(mm(.3));t.SetViaType(k.VIATYPE_THROUGH);t.SetLayerPair(k.F_Cu,k.B_Cu);t.SetNet(ground);t.SetLocked(True)
                t.SetFrontTentingMode(k.TENTING_MODE_NOT_TENTED);t.SetBackTentingMode(k.TENTING_MODE_NOT_TENTED);b.Add(t);holes.append(hole)
            completed.add(identity);connected_points.append((layer,origin))
            break
    failed=[p.GetParentFootprint().GetReference()+'.'+p.GetNumber() for p in groundpads if (p.GetParentFootprint().GetReference(),p.GetNumber()) not in completed]
    if failed:raise RuntimeError('No legal ground escape: '+', '.join(failed))
