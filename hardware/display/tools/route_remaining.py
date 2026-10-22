#!/usr/bin/env python3
"""Finish short residual connections on a conservative grid; KiCad DRC is final.

This is a layout-generation aid, not a substitute for the release ERC/DRC audit.
Uses only enabled signal layers; In1 remains reserved on four-layer boards.
Via-in-pad is prohibited.
"""
import heapq,math,json,subprocess,sys
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
import pcbnew as k
from design import ROOT, board_outline
k.SwigPyIterator.next=k.SwigPyIterator.__next__
GRID=.05
LAYERS=[k.F_Cu,k.In2_Cu,k.B_Cu]
def mm(v):return k.FromMM(v)
def xy(x,y):return k.VECTOR2I(mm(x),mm(y))

def route(b,which,entry,width=.15):
    via_radius = .275 if which == "display-adapter" else .325
    via_drill = .25 if which == "display-adapter" else .3
    clearance = .125 if which == 'display-adapter' else .15
    global LAYERS
    LAYERS=[k.F_Cu,k.B_Cu] if b.GetCopperLayerCount()==2 else [k.F_Cu,k.In2_Cu,k.B_Cu]
    items={t.m_Uuid.AsString():t for t in b.GetTracks()}
    items.update({p.m_Uuid.AsString():p for f in b.GetFootprints() for p in f.Pads()})
    if any(i['uuid'] not in items for i in entry['items']):return False
    a,c=[items[i['uuid']] for i in entry['items']]
    net=a.GetNet();name=a.GetNetname()
    # Connector pad escapes stay no wider than their copper.
    if any(isinstance(t,k.PAD) and min(t.GetSize().x,t.GetSize().y)<mm(.8) for t in [a,c]):width=.15
    w,h=(55,50) if which=='display-adapter' else (122,53.34)
    W,H=math.ceil(w/GRID)+1,math.ceil(h/GRID)+1
    images=[Image.new('1',(W,H)) for _ in LAYERS];vi=Image.new('1',(W,H))
    def px(p):return (k.ToMM(p.x)/GRID,k.ToMM(p.y)/GRID)
    def circle(draw,p,r):
        x,y=px(p);r/=GRID;draw.ellipse((x-r,y-r,x+r,y+r),fill=1)
    def segment(draw,p,q,r):
        draw.line([px(p),px(q)],fill=1,width=math.ceil(2*r/GRID));circle(draw,p,r);circle(draw,q,r)
    def pad(draw,p,inflate):
        poly=p.GetEffectivePolygon(p.GetLayer());line=poly.COutline(0)
        pts=[px(line.CPoint(j)) for j in range(line.PointCount())]
        draw.polygon(pts,fill=1)
        draw.line(pts+[pts[0]],fill=1,width=math.ceil(2*inflate/GRID),joint='curve')
        for j in range(line.PointCount()):circle(draw,line.CPoint(j),inflate)
    draws=[ImageDraw.Draw(im) for im in images];vd=ImageDraw.Draw(vi)
    # Exposed vias must stay outside the DF23 contact bands, including the
    # automatically enlarged solder-mask aperture (20um per side).
    if which=='display-adapter':
        for y0,y1 in [(21.5,22.2),(23.8,24.5)]:
            vd.rectangle(((21-via_radius-.03)/GRID,(y0-via_radius-.03)/GRID,
                          (34+via_radius+.03)/GRID,(y1+via_radius+.03)/GRID),fill=1)
    # The 505110 contact mechanism is close to the PCB. Do not rely on
    # solder mask to insulate exposed vias anywhere under its body.
    envelope=(9.95,1.35,45.05,7.0) if which=='display-adapter' else (115.0,8.95,120.65,44.05)
    x0,y0,x1,y1=envelope
    vd.rectangle(((x0-via_radius-.03)/GRID,(y0-via_radius-.03)/GRID,
                  (x1+via_radius+.03)/GRID,(y1+via_radius+.03)/GRID),fill=1)
    margin=clearance+width/2+.04
    pads=[p for f in b.GetFootprints() for p in f.Pads()]
    for p in pads:
        if p.GetNetname()!=name:
            for l,d in zip(LAYERS,draws):
                if p.IsOnLayer(l):pad(d,p,max(margin,k.ToMM(p.GetLocalClearance() or 0)+width/2+.01))
            pad(vd,p,via_radius+max(.15,k.ToMM(p.GetLocalClearance() or 0))+.04)
        else:pad(vd,p,via_radius+.025) # no via-in-pad, even on same net
        if p.GetDrillSize().x:
            r=max(p.GetDrillSize().x,p.GetDrillSize().y)/2/1e6
            circle(vd,p.GetPosition(),r+.15+.30+.01)
            if p.GetNetname()!=name:
                for d in draws:circle(d,p.GetPosition(),r+max(clearance,.3)+width/2+.06)
    for t in b.GetTracks():
        if isinstance(t,k.PCB_VIA):
            if t.GetNetname()!=name:
                for d in draws:circle(d,t.GetPosition(),k.ToMM(t.GetWidth(k.F_Cu))/2+margin)
                circle(vd,t.GetPosition(),k.ToMM(t.GetWidth(k.F_Cu))/2+via_radius+.15+.04)
            circle(vd,t.GetPosition(),k.ToMM(t.GetDrillValue())/2+.15+.30+.01)
        elif t.GetNetname()!=name:
            r=k.ToMM(t.GetWidth())/2
            if t.GetLayer() in LAYERS:segment(draws[LAYERS.index(t.GetLayer())],t.GetStart(),t.GetEnd(),r+margin)
            segment(vd,t.GetStart(),t.GetEnd(),r+via_radius+.15+.04)
    contour=board_outline(which)
    for im,edge in [(i,.3+width/2+.01) for i in images]+[(vi,.635)]:
        outside=Image.new('1',(W,H),1);d=ImageDraw.Draw(outside);pts=[(x/GRID,y/GRID) for x,y in contour];d.polygon(pts,fill=0);d.line(pts+[pts[0]],fill=1,width=math.ceil(2*edge/GRID),joint='curve')
        im.paste(1,mask=outside)
    blocked=np.stack([np.asarray(im,dtype=bool) for im in images]);via_block=np.array(vi,dtype=bool)
    # Existing plated connections already join layers; they require no new via.
    plated={}
    for t in [*pads,*list(b.GetTracks())]:
        if t.GetNetname()!=name:continue
        if isinstance(t,k.PCB_VIA) or isinstance(t,k.PAD) and t.GetDrillSize().x:
            p=t.GetPosition();xx=round(k.ToMM(p.x)/GRID);yy=round(k.ToMM(p.y)/GRID)
            if 0<=xx<W and 0<=yy<H:via_block[yy,xx]=False;plated[xx,yy]=p
    def pos(item):
        p=item.GetPosition() if isinstance(item,(k.PAD,k.PCB_VIA)) else item.GetStart()
        return p,round(k.ToMM(p.x)/GRID),round(k.ToMM(p.y)/GRID)
    def levels(item):return [i for i,l in enumerate(LAYERS) if item.IsOnLayer(l)]
    def anchors(seed):
        seen=set();pending=[seed];result={};cn=b.GetConnectivity()
        while pending:
            item=pending.pop();uid=item.m_Uuid.AsString()
            if uid in seen:continue
            seen.add(uid)
            points=[item.GetPosition()] if isinstance(item,(k.PAD,k.PCB_VIA)) else [item.GetStart(),item.GetEnd()]
            for p in points:
                xx=round(k.ToMM(p.x)/GRID);yy=round(k.ToMM(p.y)/GRID)
                for l in levels(item):
                    if 0<=xx<W and 0<=yy<H:
                        result[l,yy,xx]=p;blocked[l,yy,xx]=False
            pending += [t for t in cn.GetConnectedItems(item) if t.GetNetname()==name and isinstance(t,(k.PAD,k.PCB_TRACK,k.PCB_VIA)) and t.m_Uuid.AsString() not in seen]
        return result
    start_points=anchors(a);target_points=anchors(c)
    starts=list(start_points);targets=set(target_points)
    np.savez('/tmp/route-debug.npz',blocked=blocked,via_block=via_block,starts=np.array(starts),targets=np.array(list(targets)))
    if not starts or not targets:
        print('Blocked endpoint',name,starts,targets,flush=True);return False
    goals={(y,x) for l,y,x in targets}
    def heuristic(y,x):return min(math.hypot(x-cx,y-cy) for cy,cx in goals)
    queue=[];cost={};previous={}
    for p in starts:cost[p]=0;heapq.heappush(queue,(heuristic(p[1],p[2]),0,p))
    end=None;expanded=0
    directions=[(0,1,1),(0,-1,1),(1,0,1),(-1,0,1),(1,1,1.414214),(1,-1,1.414214),(-1,1,1.414214),(-1,-1,1.414214)]
    while queue:
        _,g,p=heapq.heappop(queue)
        if g!=cost.get(p):continue
        if p in targets:end=p;break
        expanded+=1
        if expanded>1200000:break
        l,y,x=p
        options=[]
        for dy,dx,dist in directions:
            yy,xx=y+dy,x+dx
            if 0<=yy<H and 0<=xx<W and not blocked[l,yy,xx] and not blocked[l,y,xx] and not blocked[l,yy,x]:options.append(((l,yy,xx),dist))
        if not via_block[y,x]:
            options += [((ll,y,x),24) for ll in range(len(LAYERS)) if ll!=l and not blocked[ll,y,x]]
        for q,d in options:
            ng=g+d
            if ng<cost.get(q,float('inf')):
                cost[q]=ng;previous[q]=p;heapq.heappush(queue,(ng+1.08*heuristic(q[1],q[2]),ng,q))
    if end is None:print('No path',name,'expanded',expanded,flush=True);return False
    path=[end]
    while path[-1] in previous:path.append(previous[path[-1]])
    path.reverse()
    # Remove grid staircases while retaining true 45/90 degree segments.
    # Test every traversed cell and its diagonal neighbours against the same
    # clearance-inflated raster used by the search.
    def visible(a,c):
        l,y,x=a;ll,yy,xx=c
        if l!=ll:return False
        dx,dy=xx-x,yy-y
        if dx and dy and abs(dx)!=abs(dy):return False
        steps=max(abs(dx),abs(dy));sx=(dx>0)-(dx<0);sy=(dy>0)-(dy<0)
        for n in range(1,steps+1):
            nx,ny=x+n*sx,y+n*sy
            if blocked[l,ny,nx]:return False
            if sx and sy and (blocked[l,ny-sy,nx] or blocked[l,ny,nx-sx]):return False
        return True
    simple=[path[0]];index=0
    while index<len(path)-1:
        limit=index+1
        while limit<len(path)-1 and path[limit+1][0]==path[index][0]:limit+=1
        chosen=index+1
        for candidate in range(limit,index+1,-1):
            if visible(path[index],path[candidate]):chosen=candidate;break
        simple.append(path[chosen]);index=chosen
    path=simple
    def track(p,q,l):
        if p==q:return
        t=k.PCB_TRACK(b);t.SetStart(p);t.SetEnd(q);t.SetWidth(mm(width));t.SetLayer(LAYERS[l]);t.SetNet(net);b.Add(t)
    track(start_points[path[0]],xy(path[0][2]*GRID,path[0][1]*GRID),path[0][0])
    j=0
    while j<len(path)-1:
        l,y,x=path[j];ll,yy,xx=path[j+1]
        if l!=ll:
            if (x,y) in plated:
                track(xy(x*GRID,y*GRID),plated[x,y],l);track(plated[x,y],xy(x*GRID,y*GRID),ll);j+=1;continue
            t=k.PCB_VIA(b);t.SetPosition(xy(x*GRID,y*GRID));t.SetWidth(mm(2*via_radius));t.SetDrill(mm(via_drill));t.SetViaType(k.VIATYPE_THROUGH);t.SetLayerPair(k.F_Cu,k.B_Cu);t.SetNet(net);t.SetFrontTentingMode(k.TENTING_MODE_NOT_TENTED);t.SetBackTentingMode(k.TENTING_MODE_NOT_TENTED);b.Add(t);j+=1;continue
        dx,dy=xx-x,yy-y;endidx=j+1
        while endidx+1<len(path):
            nn,ny,nx=path[endidx+1];pl,py,pxx=path[endidx]
            if nn!=l or nx-pxx!=dx or ny-py!=dy:break
            endidx+=1
        track(xy(x*GRID,y*GRID),xy(path[endidx][2]*GRID,path[endidx][1]*GRID),l);j=endidx
    track(xy(path[-1][2]*GRID,path[-1][1]*GRID),target_points[path[-1]],path[-1][0])
    print('Routed',name,'length grid',len(path),'expanded',expanded,flush=True);return True

def main(which):
    path=ROOT/which/(which+'.kicad_pcb');report=Path('/tmp')/(which+'-finish-drc.json')
    b=k.LoadBoard(str(path))
    for attempt in range(140):
        b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones());k.SaveBoard(str(path),b)
        subprocess.run(['kicad-cli','pcb','drc','--format','json','-o',str(report),str(path)],check=True,stdout=subprocess.DEVNULL)
        r=json.loads(report.read_text());missing=r['unconnected_items']
        print('Remaining',len(missing),'violations',len(r['violations']),flush=True)
        if not missing:break
        missing.sort(key=lambda e:0 if any(s in e['items'][0]['description'] for s in ['MCU_R3','MCU_G6']) else 1)
        if not any(route(b,which,e, .6 if '+24V_' in e['items'][0]['description'] else .15) for e in missing):break

if __name__=='__main__':main(sys.argv[1])
