#!/usr/bin/env python3
"""Audit duplicate copper, unused signal paths/vias and ground/power via roles.

A per-layer geometry graph checks all physical pads. Centred signal junctions
are preserved; mere overlap must not bypass full-width corner connections.
Power/ground stitching is not reduced to a spanning tree: it serves current,
thermal and return-path purposes. Covered copper and duplicate tracks are still
checked on every net. Suspected pad-entry segments are additionally tested by
removal in a scratch KiCad project. Short centred pad terminations are kept
even if their removal would leave enough edge overlap to pass DRC.
This tool never edits the released PCB.
"""
import json,math,sys
from collections import defaultdict
from pathlib import Path
import pcbnew as k
from shapely.geometry import Polygon,Point,LineString
from shapely.ops import unary_union
from shapely.strtree import STRtree
k.SwigPyIterator.next=k.SwigPyIterator.__next__
LAYERS=[k.F_Cu,k.In1_Cu,k.In2_Cu,k.B_Cu]
def xy(p):return (p.x/1e6,p.y/1e6)
def polygon_set(ps):
 def loop(s):return [xy(s.CPoint(i)) for i in range(s.PointCount())]
 return [Polygon(loop(ps.COutline(i)),[loop(ps.CHole(i,j)) for j in range(ps.HoleCount(i))]).buffer(0) for i in range(ps.OutlineCount())]

def isolated_zone_fragments(nodes, adjacency, active):
 """Find trace components connected only to one existing filled polygon.

 A polygon approximation can leave a tiny trace-cap sliver outside a pour,
 defeating full-containment checks. Preserve pad/via entries and bridges
 between distinct polygons; report isolated trace fragments for removal trials.
 """
 pending={i for i in active if nodes[i]['kind']=='track'};result=[]
 while pending:
  component=set();todo=[next(iter(pending))]
  while todo:
   i=todo.pop()
   if i not in pending:continue
   pending.remove(i);component.add(i)
   todo.extend(j for j in adjacency.get(i,set()) if j in pending)
  neighbours={j for i in component for j in adjacency.get(i,set())
              if j in active and j not in component}
  if len(neighbours)==1 and nodes[next(iter(neighbours))]['kind']=='zone':
   result.extend(sorted(component))
 return result

def scan(path):
 b=k.LoadBoard(str(path));nodes=[];nets=defaultdict(list);shapes=defaultdict(list);zone_union=defaultdict(list)
 def add(kind,obj,net,geo,label):
  i=len(nodes);nodes.append(dict(kind=kind,obj=obj,net=net,geo=geo,label=label));nets[net].append(i)
  for l,g in geo.items():shapes[net,l].append((i,g))
 for f in b.GetFootprints():
  for p in f.Pads():
   if not p.GetNetCode():continue
   geo={l:unary_union(polygon_set(p.GetEffectivePolygon(l))) for l in LAYERS if p.IsOnLayer(l)}
   add('pad',p,p.GetNetname(),geo,f.GetReference()+'.'+p.GetNumber())
 for z in b.Zones():
  if z.GetIsRuleArea() or not z.GetNetCode():continue
  for l in LAYERS:
   if not z.IsOnLayer(l):continue
   for j,g in enumerate(polygon_set(z.GetFilledPolysList(l))):
    add('zone',z,z.GetNetname(),{l:g},'zone:'+str(l)+':'+str(j));zone_union[z.GetNetname(),l].append(g)
 for t in b.GetTracks():
  net=t.GetNetname()
  if isinstance(t,k.PCB_VIA):
   geo={l:Point(xy(t.GetPosition())).buffer(t.GetWidth(l)/2e6,quad_segs=32) for l in LAYERS if t.IsOnLayer(l)};kind='via'
  else:
   assert not isinstance(t,k.PCB_ARC)
   geo={t.GetLayer():LineString([xy(t.GetStart()),xy(t.GetEnd())]).buffer(t.GetWidth()/2e6,quad_segs=32)};kind='track'
  add(kind,t,net,geo,t.m_Uuid.AsString())
 adj=defaultdict(set);adj_layers=defaultdict(set);fallback=[]
 for (net,l),entries in shapes.items():
  ids,gs=zip(*entries);tree=STRtree(gs)
  for a,g in enumerate(gs):
   for c in tree.query(g.buffer(.000002)):
    if c<=a or g.distance(gs[c])>.000002:continue
    u,v=ids[a],ids[c]
    nu,nv=nodes[u],nodes[v]
    # Retain full-width, centred track/via junctions. A secondary graph
    # records existing edge-connected joints for explicit alignment review.
    def centre(n):
     t=n['obj']
     return LineString([xy(t.GetStart()),xy(t.GetEnd())]) if n['kind']=='track' else Point(xy(t.GetPosition()))
    ordinary=net in ('GND','PWR_RTN') or net.startswith('+')
    if not ordinary:
     for track,pad in [(nu,nv),(nv,nu)]:
      if track['kind']=='track' and pad['kind']=='pad':
       if not centre(track).intersects(pad['geo'][l].buffer(.000002)):
        break
     else:
      track=None
     if track is not None:continue
    if not ordinary and nu['kind'] in ('track','via') and nv['kind'] in ('track','via'):
     if centre(nu).distance(centre(nv))>.001:
      fallback.append((u,v,l,centre(nu).distance(centre(nv))))
      continue
    adj[u].add(v);adj[v].add(u);adj_layers[u,v].add(l);adj_layers[v,u].add(l)
 # Only restore edge-only joints between disconnected centred components.
 # Do not allow copper overlap to bypass short corner/centering segments.
 parent=list(range(len(nodes)))
 def find(i):
  while parent[i]!=i:parent[i]=parent[parent[i]];i=parent[i]
  return i
 for u,vs in adj.items():
  for v in vs:parent[find(u)]=find(v)
 alignment=[]
 for u,v,l,distance in sorted(fallback,key=lambda x:x[3]):
  if find(u)==find(v):continue
  parent[find(u)]=find(v);adj[u].add(v);adj[v].add(u);adj_layers[u,v].add(l);adj_layers[v,u].add(l)
  alignment.append(dict(net=nodes[u]['net'],first=nodes[u]['label'],second=nodes[v]['label'],gap_mm=distance,layer=b.GetLayerName(l)))
 active=set(range(len(nodes)));changes=[]
 def connected(terms,excluded=None):
  if len(terms)<2:return True
  seen=set();todo=[next(iter(terms))]
  while todo:
   u=todo.pop()
   if u in seen:continue
   seen.add(u);todo.extend(v for v in adj[u] if v in active and v!=excluded and v not in seen)
  return terms<=seen
 def desc(i,reason):
  n=nodes[i];t=n['obj'];d=dict(kind=n['kind'],net=n['net'],uuid=n['label'],reason=reason)
  if n['kind']=='via':d['at']=xy(t.GetPosition())
  else:d.update(start=xy(t.GetStart()),end=xy(t.GetEnd()),layer=b.GetLayerName(t.GetLayer()),width=t.GetWidth()/1e6)
  return d
 def drop(i,why):
  active.remove(i);changes.append(desc(i,why))
 # Pad entries are intentionally centred; unlike a filled plane, a pad is not
 # grounds for removing its complete incoming trace and retaining an edge touch.
 # Remove only copper fully contained by same-net filled pours/another track.
 for key,entries in shapes.items():
  net,l=key
  plane=unary_union(zone_union[key]);fixed=plane.buffer(.000002)
  tracks=sorted((i for i,g in entries if nodes[i]['kind']=='track'),key=lambda i:nodes[i]['obj'].GetLength())
  for i in tracks:
   if i not in active:continue
   geo=nodes[i]['geo'][l]
   if fixed.covers(geo):drop(i,'fully covered by same-net filled pour');continue
   for j,g in entries:
    if i==j or j not in active or nodes[j]['kind']!='track':continue
    if g.buffer(.000002).covers(geo):drop(i,'duplicate/contained in another same-layer track');break
 # Every signal trace/via must be needed to reach at least one physical pad.
 baseline={}
 for net,ids in nets.items():
  terms={i for i in ids if nodes[i]['kind']=='pad'}
  if not connected(terms):baseline[net]=[nodes[i]['label'] for i in terms];continue
  if net=='GND' or net=='PWR_RTN' or net.startswith('+'):continue
  candidates=sorted((i for i in ids if i in active and nodes[i]['kind'] in ('track','via')),
     key=lambda i:(nodes[i]['kind']=='via',nodes[i]['obj'].GetLength() if nodes[i]['kind']=='track' else sum(len(adj_layers[i,j]) for j in adj[i])),reverse=True)
  for i in candidates:
   if connected(terms,i):drop(i,'not needed for any terminal-to-terminal signal connection')
 # Check every net for isolated fragments. Unlike intentional local pad/via
 # connections, these do not join any terminal or two separate copper areas.
 for i in isolated_zone_fragments(nodes,adj,active):
  drop(i,'isolated trace fragment connected only to one filled polygon')
 # Ground/power: remove only vias with no interlayer function, not stitching/thermal pairs.
 retained=[]
 for net,ids in nets.items():
  if not (net=='GND' or net=='PWR_RTN' or net.startswith('+')):continue
  terms={i for i in ids if nodes[i]['kind']=='pad'}
  for i in ids:
   if i not in active or nodes[i]['kind']!='via':continue
   layers=set()
   for j in adj[i]&active:
    if nodes[j]['kind']!='via':layers|=adj_layers[i,j]
   if len(layers)<2 and connected(terms,i):drop(i,'no interlayer connection')
   else:retained.append(dict(net=net,at=xy(nodes[i]['obj'].GetPosition()),layers=[b.GetLayerName(l) for l in sorted(layers)],purpose='ground stitching' if net=='GND' else 'power/thermal interlayer connection'))
 assert not baseline,baseline
 assert all(connected({i for i in ids if nodes[i]['kind']=='pad'}) for ids in nets.values())
 return dict(changes=changes,alignment=alignment,retained_supply_ground_vias=retained,checked_net_count=len(nets),checked_pad_count=sum(n['kind']=='pad' for n in nodes))


def main(name):
 import hashlib,subprocess,tempfile,shutil
 from design import ROOT
 path=ROOT/name/(name+'.kicad_pcb')
 report=scan(path)
 assert not report['alignment'], ('Edge-only signal junctions need review',report['alignment'])
 # Trial removal distinguishes electrically required pad entries from short
 # centred terminations retained for robust pad entry and clean geometry.
 candidates=report.pop('changes')
 necessary=[]
 with tempfile.TemporaryDirectory(prefix='copper-trial-') as folder:
  root=Path(folder);project=root/name;project.mkdir()
  (root/'libraries').symlink_to(ROOT/'libraries',target_is_directory=True)
  for file in (ROOT/name).iterdir():
   if file.suffix in {'.kicad_pro','.kicad_dru'} or file.name=='fp-lib-table':
    shutil.copy2(file,project/file.name)
  board=k.LoadBoard(str(path));tracks={t.m_Uuid.AsString():t for t in board.GetTracks()}
  for candidate in candidates:
   assert candidate['kind']=='track', ('Unused signal via',candidate)
   assert candidate['reason']=='not needed for any terminal-to-terminal signal connection', ('Redundant covered/duplicate copper',candidate)
   item=tracks[candidate['uuid']]
   pads=[p for f in board.GetFootprints() for p in f.Pads()
         if p.GetNetCode()==item.GetNetCode() and p.IsOnLayer(item.GetLayer())]
   assert any(p.GetEffectiveShape(item.GetLayer()).Collide(q,1000)
              for p in pads for q in [item.GetStart(),item.GetEnd()]), ('Unused nonterminal branch',candidate)
   board.Remove(item)
   k.SaveBoard(str(project/path.name),board)
   board.Add(item)
   output=root/'trial.json'
   subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--all-track-errors',
                   '--format','json','-o',str(output),str(project/path.name)],
                  check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   drc=json.loads(output.read_text())
   failures=drc['violations']+drc['unconnected_items']
   relevant=[v for v in failures if v['type'] in {
       'connection_width','unconnected_items','track_dangling','track_not_centered_on_via'}]
   if not relevant:
    ends=[Point(xy(item.GetStart())),Point(xy(item.GetEnd()))]
    terminal_pads=[p for p in pads if any(
        unary_union(polygon_set(p.GetEffectivePolygon(item.GetLayer()))).buffer(.002).covers(e)
        for e in ends)]
    assert any(item.GetLength()/1e6<=math.hypot(*xy(p.GetSize()))+item.GetWidth()/1e6
               for p in terminal_pads), ('Long redundant terminal branch',candidate)
   necessary.append(dict(uuid=candidate['uuid'],net=candidate['net'],
      start=candidate['start'],end=candidate['end'],
      reason='Full-width pad entry; removal fails fresh-fill KiCad DRC' if relevant else 'Short centred pad termination; avoids relying on trace-cap/pad-edge overlap',
      removal_failures=sorted({v['type'] for v in relevant})))
 report.update(result='PASS',source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
     unnecessary_signal_vias=0,redundant_signal_paths=0,duplicate_or_covered_tracks=0,
     necessary_pad_entries=necessary,
     ground_power_policy='Retain interlayer ground stitching and thermal/current connections; each retained via must contact copper on at least two layers.',
     limitations='Geometry/connectivity verification, not measured signal integrity or physical qualification.')
 assert all(len(v['layers'])>=2 for v in report['retained_supply_ground_vias'])
 (ROOT/'manufacturing/verification'/(name+'-copper-audit.json')).write_text(json.dumps(report,indent=2)+'\n')
 print(name,'copper audit PASS;',len(necessary),'pad entries individually reviewed and trial-removed')


if __name__=='__main__':
 main(sys.argv[1])
