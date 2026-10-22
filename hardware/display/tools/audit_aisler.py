#!/usr/bin/env python3
"""Supplement AISLER templates with geometric checks, without DRC exclusions.

Requires pcbnew and Shapely. Run after zone fill. Mask calculations model AISLER's
published 20um enlargement; the actual uploaded job still requires CAM review.
"""
import hashlib
import itertools
import json
import math
import subprocess
import sys

import pcbnew as k
from shapely.geometry import Polygon, Point, LineString, box
from drill_geometry import pad_drill
from design import ROOT, BOARDS
from aisler import SUPPORT, BRIDGES
from sexpr import parse, children, child

k.SwigPyIterator.next=k.SwigPyIterator.__next__


def xy(p):return k.ToMM(p.x), k.ToMM(p.y)


def pad_polygon(p):
    shape=p.GetEffectivePolygon(p.GetLayer());c=shape.COutline(0)
    return Polygon([xy(c.CPoint(i)) for i in range(c.PointCount())])


def main(name):
    path=ROOT/name/(name+'.kicad_pcb');b=k.LoadBoard(str(path))
    ds=b.GetDesignSettings();fps=b.GetFootprints();tracks=b.GetTracks()
    assert abs(k.ToMM(ds.GetBoardThickness())-1.6)<1e-6
    assert b.GetCopperLayerCount()==BOARDS[name]['layers']
    assert ds.m_SolderMaskExpansion==0
    source=parse(path.read_text());stack=child(child(source,'setup'),'stackup')
    assert child(stack,'copper_finish')[1]=='ENIG'
    thickness={l[1]:float(child(l,'thickness')[1]) for l in children(stack,'layer') if child(l,'thickness')}
    assert all(thickness[n]==.035 for n in ['F.Cu','B.Cu'])
    if BOARDS[name]['layers']==4:
        assert [thickness['dielectric '+str(i)] for i in [1,2,3]]==[.14,1.20,.14]
        assert thickness['In1.Cu']==thickness['In2.Cu']==.035
    rings=[];smd=[];holes=[];vias=[];mask_webs=[];slots=[]
    for f in fps:
        pp=list(f.Pads())
        for p in pp:
            assert not p.GetLocalSolderMaskMargin(),(f.GetReference(),p.GetNumber(),'mask override')
            drill=k.ToMM(p.GetDrillSize().x)
            if drill:
                a,c,diameter=pad_drill(p)
                shape=(Point(a) if a==c else LineString([a,c])).buffer(diameter/2,quad_segs=128)
                holes.append(shape)
                if a!=c:
                    length=math.dist(a,c)+diameter
                    assert p.GetAttribute()==k.PAD_ATTRIB_PTH,'NPTH slots need an outline review'
                    assert .7<=diameter<=2.7 and length>1.5*diameter
                    slots.append(dict(pad=f.GetReference()+'.'+p.GetNumber(),width_mm=diameter,length_mm=length))
                if p.GetAttribute()==k.PAD_ATTRIB_PTH:
                    if a==c:
                        # KiCad's polygon approximation inscribes circular pads.
                        # Use exact dimensions for centred round holes instead.
                        assert p.GetOffset().x==p.GetOffset().y==0
                        assert p.GetShape() in (k.PAD_SHAPE_CIRCLE,k.PAD_SHAPE_OVAL,k.PAD_SHAPE_RECT,k.PAD_SHAPE_ROUNDRECT)
                        ring=(min(xy(p.GetSize()))-diameter)/2
                    else:
                        polygon=pad_polygon(p)
                        assert polygon.covers(shape),(f.GetReference(),p.GetNumber(),'hole outside pad')
                        ring=polygon.boundary.distance(shape)
                    rings.append(ring)
                    assert ring>=.3-2e-5,(f.GetReference(),p.GetNumber(),ring)
            elif p.GetAttribute()==k.PAD_ATTRIB_SMD:
                size=min(xy(p.GetSize()));smd.append(size)
                assert size>=.2-1e-6,(f.GetReference(),p.GetNumber(),size)
        # Distinct fine-pitch contacts must retain a 100um processed mask web.
        pads=[p for p in pp if p.GetAttribute()==k.PAD_ATTRIB_SMD and p.GetNumber()]
        for a,c in itertools.combinations(pads,2):
            if a.GetNumber()==c.GetNumber():continue
            gap=pad_polygon(a).distance(pad_polygon(c))-.04
            if gap<.8:
                mask_webs.append(gap)
                assert gap>=.1-1e-5,(name,f.GetReference(),a.GetNumber(),c.GetNumber(),'processed mask web',gap)
    for v in tracks:
        if not isinstance(v,k.PCB_VIA):continue
        assert v.GetViaType()==k.VIATYPE_THROUGH
        assert v.GetFrontTentingMode()==k.TENTING_MODE_NOT_TENTED
        assert v.GetBackTentingMode()==k.TENTING_MODE_NOT_TENTED
        drill=k.ToMM(v.GetDrillValue());diam=k.ToMM(v.GetWidth(k.F_Cu))
        assert drill>=.25-1e-6 and (diam-drill)/2>=.1-1e-6
        holes.append(Point(xy(v.GetPosition())).buffer(drill/2,quad_segs=128))
        vias.append(v)
    # Include neighbouring exposed vias, not only pad-to-pad gaps, around
    # the fine-pitch connector contacts. Model the same 20um CAM enlargement.
    connector_refs={'J1','J2'} if name=='display-adapter' else {'J1'}
    contact_via_webs=[]
    for f in fps:
        if f.GetReference() not in connector_refs:continue
        for p in f.Pads():
            if not p.GetNumber().isdigit():continue
            polygon=pad_polygon(p)
            for v in vias:
                gap=polygon.distance(Point(xy(v.GetPosition())).buffer(k.ToMM(v.GetWidth(k.F_Cu))/2))-.04
                contact_via_webs.append(gap)
                assert gap>=.1-1e-5,('connector/via processed mask web',f.GetReference(),p.GetNumber(),gap)
    spacing=min(a.distance(c) for a,c in itertools.combinations(holes,2))
    assert spacing>=.3-1e-5,('hole-to-hole',spacing)
    assert sum(s['length_mm'] for s in slots)<=75,'AISLER nibbled-slot fair-use length'
    # Manufacturer DF23 drawing EDC3-164006-03 Fig1 note3; conservative bands.
    if name=='display-adapter':
        bands=[box(21,y0,34,y1) for y0,y1 in [(21.5,22.2),(23.8,24.5)]]
        for v in vias:
            aperture=Point(xy(v.GetPosition())).buffer(k.ToMM(v.GetWidth(k.B_Cu))/2+.02)
            assert all(not aperture.intersects(z) for z in bands),('exposed DF23 via',xy(v.GetPosition()))
    connector=next(f for f in fps if f.GetReference()==('J2' if name=='display-adapter' else 'J1'))
    cx,cy=xy(connector.GetPosition())
    envelope=box(cx-17.55,cy-5.65,cx+17.55,cy) if name=='display-adapter' else box(cx,cy-17.55,cx+5.65,cy+17.55)
    for v in vias:
        aperture=Point(xy(v.GetPosition())).buffer(k.ToMM(v.GetWidth(k.F_Cu))/2+.02)
        assert not aperture.intersects(envelope),('exposed Molex body via',xy(v.GetPosition()))
    silk=[]
    for item in [*b.GetDrawings(),*[i for f in fps for i in f.GraphicalItems()],
                 *[i for f in fps for i in f.GetFields()]]:
        if item.GetLayer() not in (k.F_SilkS,k.B_SilkS):continue
        if isinstance(item,k.PCB_TEXT):
            if not item.IsVisible():continue
            assert k.ToMM(item.GetTextHeight())>=.8-1e-6
            assert k.ToMM(item.GetTextThickness())>=.15-1e-6
        elif isinstance(item,k.PCB_SHAPE):
            assert k.ToMM(item.GetWidth())>=.15-1e-6
        silk.append(item)
    markers=[i for i in b.GetDrawings() if i.GetLayer()==k.User_1]
    assert b.GetLayerName(k.User_1)=='aisler-bridges'
    assert len(markers)==len(BRIDGES[name])
    assert all(abs(k.ToMM(i.GetWidth())-.05588)<1e-6 for i in markers)
    # The accepted 1.8mm milling tool needs 0.9mm concave fillets.
    arcs=[i for i in b.GetDrawings() if i.GetLayer()==k.Edge_Cuts and isinstance(i,k.PCB_SHAPE) and i.GetShape()==k.SHAPE_T_ARC]
    if name=='giga-adapter':assert arcs and all(k.ToMM(i.GetRadius())>=.89999 for i in arcs)
    project=json.loads(path.with_suffix('.kicad_pro').read_text())
    assert not project['board']['design_settings']['drc_exclusions']
    assert not project['erc']['erc_exclusions']
    lock=json.loads((ROOT/'manufacturing/aisler-support-lock.json').read_text())
    for relative,expected in lock['template_sha256'].items():
        assert hashlib.sha256((SUPPORT/relative).read_bytes()).hexdigest()==expected,relative
    revision=lock['revision']
    if (SUPPORT/'.git').exists():
        assert subprocess.check_output(['git','-C',str(SUPPORT),'rev-parse','HEAD'],text=True).strip()==revision
    report=dict(result='PASS',board=name,revision=BOARDS[name]['revision'],support_revision=revision,
                template=BOARDS[name]['template'],source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                finished_thickness_mm=1.6,finished_thickness_tolerance_percent=10,copper_um=35,finish='ENIG',
                minimum_component_annular_ring_mm=min(rings),minimum_smd_dimension_mm=min(smd),
                minimum_hole_spacing_mm=spacing,minimum_processed_fine_pitch_mask_web_mm=min(mask_webs),
                minimum_processed_connector_to_via_mask_web_mm=min(contact_via_webs),
                through_vias=len(vias),plated_slots=slots,tenting='NONE',silk_items_checked=len(silk),bridge_markers=len(markers),
                stackup_mm=thickness,manufacturing_waivers=[],
                connector_review='DF23 note-3 bands exclude exposed via apertures. Molex 505110: no exposed vias inside the connector body or mounting lands; fine-pitch mask webs checked.',
                aisler_processed_job='PENDING — actual CAM geometry/options/bridges/netlist must be reviewed after upload',
                physical_qualification='NOT PERFORMED')
    (ROOT/'manufacturing/verification'/(name+'-aisler-audit.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(name,'AISLER geometry PASS')


if __name__=='__main__':main(sys.argv[1])
