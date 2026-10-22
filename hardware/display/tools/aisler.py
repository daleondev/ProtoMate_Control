#!/usr/bin/env python3
"""AISLER manufacturing settings shared by generation, migration and release.

The pinned upstream templates are a baseline, not a complete manufacturing DRC.
Current published limits and this project's conservative routing policy supplement
them. --apply updates settings/footprints without discarding reviewed routing.
"""
import argparse
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import pcbnew as k
from design import ROOT, BOARDS, board_outline, components

k.SwigPyIterator.next = k.SwigPyIterator.__next__
SUPPORT = ROOT / 'aisler-support' if (ROOT/'aisler-support').is_dir() else ROOT.parent / 'aisler-support'
# Keep detached SWIG proxies alive until the board is saved (KiCad 10 bindings).
_detached = []
BRIDGES = {
    'display-adapter': [((-.4,18),(-1.8,18)), ((55.4,18),(56.8,18)),
                        ((3,-.4),(3,-1.8)), ((38,50.4),(38,51.8))],
    'giga-adapter': [((35,-.4),(35,-1.8)), ((85,-.4),(85,-1.8)),
                    ((35,53.74),(35,55.14)), ((85,53.74),(85,55.14)),
                    ((10.6,50),(9.2,50)), ((122.4,26),(123.8,26))],
}


def point(x, y):
    return k.VECTOR2I(k.FromMM(x), k.FromMM(y))


def configure_project(name, directory=None):
    directory = directory or ROOT / name
    template = BOARDS[name]['template']
    source = SUPPORT / 'kicad' / template
    profile = json.loads((source / (template + '.kicad_pro')).read_text())
    path = directory / (name + '.kicad_pro')
    project = json.loads(path.read_text()) if path.exists() else {}
    settings = project.setdefault('board', {}).setdefault('design_settings', {})
    settings.setdefault('rules', {}).update(profile['board']['design_settings']['rules'])
    settings['rules'].update(min_silk_clearance=.125, min_text_height=.8,
                             min_text_thickness=.15, min_silk_text_height=.8,
                             min_silk_text_thickness=.15, min_hole_to_hole=.3)
    settings.setdefault('defaults', {}).update(
        silk_line_width=.15, silk_text_thickness=.15)
    settings.setdefault('drc_exclusions', [])
    # Keep the project's stricter severities. The old template ignores several
    # useful checks; importing those would hide unrelated layout problems.
    severity = settings.setdefault('rule_severities', {})
    for key in ['annular_width', 'hole_to_hole', 'items_not_allowed', 'solder_mask_bridge']:
        severity[key] = 'error'
    net = project.setdefault('net_settings', {'meta': {'version': 5}})
    net.setdefault('classes', [dict(name='Default', clearance=.15, track_width=.18,
                                    via_diameter=.65, via_drill=.3)])
    if name == 'display-adapter':
        # HD clearance is needed only for the new escape traces between existing
        # staggered DF23 vias; widths and via sizes remain unchanged.
        for cls in net['classes']:
            cls.update(clearance=.125,track_width=.15,via_diameter=.55,via_drill=.25)
    project.setdefault('erc', {}).setdefault('erc_exclusions', [])
    path.write_text(json.dumps(project, indent=2) + '\n')
    custom = (source / (template + '.kicad_dru')).read_text()
    custom += '''
# Project supplement: AISLER ENIG rules, checked 2026-10-21.
# Component holes require a larger ring than vias (not captured by templates).
(rule "AISLER component hole annular ring"
  (condition "A.Type == 'Pad' && A.Pad_Type == 'Through-hole'")
  (constraint annular_width (min 0.30mm)))
'''
    (directory / (name + '.kicad_dru')).write_text(custom)


def normalize_footprint(f):
    """Apply identically to local library footprints and their placed instances."""
    for p in f.Pads():
        p.SetLocalSolderMaskMargin(0)
        if 'GIGA_J8' in str(f.GetFPID().GetLibItemName()) or f.GetReference() == 'J8':
            p.SetSize(point(1.70, 1.70))
        if 'TRACO_TSR-1_THT' in str(f.GetFPID().GetLibItemName()):
            size = p.GetSize()
            p.SetSize(k.VECTOR2I(max(size.x, k.FromMM(1.70)), size.y))
    for item in f.GraphicalItems():
        if item.GetLayer() in (k.F_SilkS, k.B_SilkS) and isinstance(item, k.PCB_SHAPE):
            item.SetWidth(max(item.GetWidth(), k.FromMM(.15)))
    # Dense body outlines overlap open via apertures or the larger silk stroke.
    # Keep the full outlines on Fab and an unambiguous pin-1/cathode dot on silk.
    sparse = {
        'TSSOP-24_4.4x7.8mm_P0.65mm': (-1.7, -3.2),
        'SOT-23-6': (-.15, -.45), 'SOT-23-6_UpperPin1': (-.15, -.85), 'D_SMA': (-.3, 0), 'D_SMB': (-.4, 0),
        'Converter_DCDC_TRACO_TSR2-xxxx_THT': (-2.4, -3.4),
        'R_0603_1608Metric': None, 'R_0402_1005Metric': None,
        'C_1210_3225Metric': None,
        'TSSOP-14_4.4x5mm_P0.65mm': None,
        'WSON-10-1EP_2.5x2.5mm_P0.5mm_EP1.2x2mm': None,
    }
    key = str(f.GetFPID().GetLibItemName())
    if key in sparse:
        for item in f.GraphicalItems():
            if (sparse[key] and isinstance(item,k.PCB_SHAPE) and item.GetShape()==k.SHAPE_T_CIRCLE
                    and item.GetLayer() in (k.F_SilkS,k.B_SilkS)):
                item.SetCenter(point(*sparse[key]));item.SetEnd(point(sparse[key][0]+.18,sparse[key][1]))
                item.Rotate(point(0,0),f.GetOrientation());item.Move(f.GetPosition())
        moved = False
        for item in f.GraphicalItems():
            if (isinstance(item, k.PCB_SHAPE) and item.GetLayer() in (k.F_SilkS, k.B_SilkS)
                    and item.GetShape() != k.SHAPE_T_CIRCLE):
                item.SetLayer(k.B_Fab if f.IsFlipped() else k.F_Fab)
                moved = True
        if moved and sparse[key] and not any(isinstance(i,k.PCB_SHAPE) and
                i.GetShape()==k.SHAPE_T_CIRCLE and i.GetLayer() in (k.F_SilkS,k.B_SilkS)
                for i in f.GraphicalItems()):
            s=k.PCB_SHAPE(f);s.SetShape(k.SHAPE_T_CIRCLE)
            s.SetCenter(point(*sparse[key]));s.SetEnd(point(sparse[key][0]+.18,sparse[key][1]))
            s.SetWidth(k.FromMM(.15));s.SetLayer(k.F_SilkS)
            s.Rotate(point(0,0),f.GetOrientation());s.Move(f.GetPosition());f.Add(s)


def outline(b, name):
    """Round only concave corners to suit the 1.8mm routing tool."""
    points = board_outline(name)
    corners = []
    for i, p in enumerate(points):
        a, c = points[i-1], points[(i+1) % len(points)]
        u = (p[0]-a[0], p[1]-a[1]); v = (c[0]-p[0], c[1]-p[1])
        if u[0]*v[1]-u[1]*v[0] >= 0:
            corners.append((p,p,None)); continue
        u = tuple(x/math.hypot(*u) for x in u)
        v = tuple(x/math.hypot(*v) for x in v)
        start = tuple(p[j]-.9*u[j] for j in range(2))
        end = tuple(p[j]+.9*v[j] for j in range(2))
        centre = tuple(start[j]+.9*v[j] for j in range(2))
        mid = tuple(centre[j]+((start[j]-centre[j])+(end[j]-centre[j]))/math.sqrt(2) for j in range(2))
        corners.append((start,end,mid))
    for item in list(b.GetDrawings()):
        if item.GetLayer() == k.Edge_Cuts:
            b.Remove(item)
            _detached.append(item)
    for i, (start,end,mid) in enumerate(corners):
        s = k.PCB_SHAPE(b);s.SetShape(k.SHAPE_T_SEGMENT)
        s.SetStart(point(*corners[i-1][1]));s.SetEnd(point(*start))
        s.SetLayer(k.Edge_Cuts);s.SetWidth(k.FromMM(.05));b.Add(s)
        if mid:
            s = k.PCB_SHAPE(b);s.SetShape(k.SHAPE_T_ARC)
            s.SetArcGeometry(point(*start),point(*mid),point(*end))
            s.SetLayer(k.Edge_Cuts);s.SetWidth(k.FromMM(.05));b.Add(s)


def board_settings(b, name):
    ds = b.GetDesignSettings()
    ds.SetBoardThickness(k.FromMM(BOARDS[name]['thickness']))
    ds.m_SolderMaskExpansion = 0
    ds.m_SolderMaskMinWidth = k.FromMM(.1)
    parts = {p['ref']:p for p in components(name)}
    for f in b.GetFootprints():
        normalize_footprint(f)
        part=parts.get(f.GetReference())
        if part:
            if part['assembly']=='PCB':
                f.SetAttributes(f.GetAttributes() | k.FP_EXCLUDE_FROM_BOM | k.FP_EXCLUDE_FROM_POS_FILES)
            for field,key in [('Manufacturer','mfr'),('MPN','mpn'),('Assembly','assembly')]:
                f.SetField(field,part[key]);f.GetField(field).SetVisible(False)
            for p in f.Pads():
                if p.GetNumber() in part['nets'] and not part['nets'][p.GetNumber()]:
                    netname=f'unconnected-({f.GetReference()}-Pad{p.GetNumber()})'
                    if name=='giga-adapter' and f.GetReference()=='U7':
                        pinname={'4':'NC','7':'SHDN','10':'IMON','13':'NC','14':'FLT'}[p.GetNumber()]
                        netname=f'unconnected-(U7-{pinname}-Pad{p.GetNumber()})'
                    if name=='display-adapter' and f.GetReference()=='U3' and p.GetNumber()=='5':
                        netname='unconnected-(U3-PG-Pad5)'
                    net=b.FindNet(netname)
                    if not net:
                        net=k.NETINFO_ITEM(b,netname);b.Add(net)
                    p.SetNet(net)
        elif f.GetReference().startswith('H'):
            f.SetAttributes(f.GetAttributes() | k.FP_BOARD_ONLY)
    for v in b.GetTracks():
        if isinstance(v, k.PCB_VIA):
            v.SetFrontTentingMode(k.TENTING_MODE_NOT_TENTED)
            v.SetBackTentingMode(k.TENTING_MODE_NOT_TENTED)
    outline(b, name)
    if name == 'display-adapter' and not any(z.GetZoneName().startswith('DF23 exposed') for z in b.Zones()):
        for y0,y1 in [(21.5,22.2),(23.8,24.5)]:
            z=k.ZONE(b);z.SetLayer(k.B_Cu);z.SetIsRuleArea(True)
            z.SetDoNotAllowVias(True);z.SetDoNotAllowTracks(False)
            z.SetDoNotAllowPads(False);z.SetDoNotAllowZoneFills(False);z.SetDoNotAllowFootprints(False)
            z.SetZoneName('DF23 exposed-contact band: no vias')
            poly=z.Outline();poly.NewOutline()
            for p in [(21,y0),(34,y0),(34,y1),(21,y1)]:poly.Append(point(*p))
            b.Add(z)
    layers = b.GetEnabledLayers();layers.AddLayer(k.User_1);b.SetEnabledLayers(layers)
    b.SetLayerName(k.User_1, 'aisler-bridges')
    for item in list(b.GetDrawings()):
        if item.GetLayer() == k.User_1:
            b.Remove(item)
            _detached.append(item)
        elif isinstance(item, k.PCB_TEXT):
            t = item.GetText()
            if t.startswith(('SMARTWIN / ', 'GIGA DISPLAY / ')):
                item.SetText(t.rsplit(' / ',1)[0] + ' / ' + BOARDS[name]['revision'])
            if name == 'display-adapter':
                locations = {('102mm TYPE-A', k.F_SilkS): (28,.75),
                             ('J1 PIN 1', k.B_SilkS): (19.75,30),
                             ('SMARTWIN / '+BOARDS[name]['revision'], k.B_SilkS): (27.5,6.75)}
                location = locations.get((item.GetText(),item.GetLayer()))
                if location:
                    item.SetPosition(point(*location))
    for start, end in BRIDGES[name]:
        s = k.PCB_SHAPE(b);s.SetShape(k.SHAPE_T_SEGMENT)
        s.SetStart(point(*start));s.SetEnd(point(*end))
        s.SetWidth(k.FromMM(.05588));s.SetLayer(k.User_1);b.Add(s)
    tb = b.GetTitleBlock();tb.SetRevision(BOARDS[name]['revision']);tb.SetDate(BOARDS[name]['date']);b.SetTitleBlock(tb)


def write_stackup(path, name):
    text = path.read_text()
    # pcbnew cannot reliably edit the complete stackup via its Python API.
    start = text.find('(stackup')
    if start >= 0:
        pos = start;depth = 0
        while pos < len(text):
            depth += (text[pos] == '(') - (text[pos] == ')')
            pos += 1
            if depth == 0:break
        text = text[:start] + text[pos:]
    layers = [('F.Cu','copper',.035,None)]
    if BOARDS[name]['layers'] == 4:
        layers += [('dielectric 1','prepreg',.14,4.3), ('In1.Cu','copper',.035,None),
                   ('dielectric 2','core',1.20,4.6), ('In2.Cu','copper',.035,None),
                   ('dielectric 3','prepreg',.14,4.3)]
    else:
        # 2L laminate is variable; this is nominal mechanical bookkeeping.
        layers += [('dielectric 1','core',1.53,4.5)]
    layers += [('B.Cu','copper',.035,None)]
    stack = '(stackup\n(layer "F.SilkS" (type "Top Silk Screen") (color "White"))\n'
    stack += '(layer "F.Mask" (type "Top Solder Mask") (color "Green"))\n'
    for label,kind,size,er in layers:
        stack += f'(layer "{label}" (type "{kind}") (thickness {size})'
        if er:stack += f' (material "FR4") (epsilon_r {er}) (loss_tangent 0.02)'
        stack += ')\n'
    stack += '(layer "B.Mask" (type "Bottom Solder Mask") (color "Green"))\n'
    stack += '(layer "B.SilkS" (type "Bottom Silk Screen") (color "White"))\n'
    stack += '(copper_finish "ENIG") (dielectric_constraints no))'
    text = text.replace('(setup\n', '(setup\n'+stack+'\n', 1)
    path.write_text('\n'.join(line.rstrip() for line in text.splitlines())+'\n')


def apply(name):
    configure_project(name)
    path = ROOT / name / (name+'.kicad_pcb')
    b = k.LoadBoard(str(path));board_settings(b, name)
    b.BuildConnectivity();k.ZONE_FILLER(b).Fill(b.Zones())
    k.SaveBoard(str(path), b);write_stackup(path,name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', required=True)
    parser.add_argument('--board', choices=list(BOARDS))
    args = parser.parse_args()
    if args.board:apply(args.board)
    else:
        for name in BOARDS:
            subprocess.run([sys.executable,__file__,'--apply','--board',name],check=True)
