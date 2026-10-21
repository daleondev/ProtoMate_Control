#!/usr/bin/env python3
"""Rebuild schematics and initial PCB placement using KiCad's pcbnew API.

Routing is maintained in the checked-in PCBs. This command deliberately writes
placement boards to build/ so it cannot accidentally discard reviewed routing.
"""
import csv
import json
import math
import re
import shutil
import uuid
from pathlib import Path
import pcbnew as k
from design import ROOT, PARTS, SIGNALS, GEOMETRY, REVISION, BOARDS, components, board_outline
from aisler import board_settings, configure_project, normalize_footprint, write_stackup

k.SwigPyIterator.next = k.SwigPyIterator.__next__
LIB = ROOT/'libraries/DisplayAdapter.pretty'
MODELS = ROOT/'libraries/3dmodels'
NAMESPACE = uuid.UUID('c608b657-3505-41f8-800c-22e8ab86ecb7')
def uid(value): return str(uuid.uuid5(NAMESPACE, value))
def q(s): return json.dumps(str(s))
def mm(v): return k.FromMM(float(v))
def point(x,y): return k.VECTOR2I(mm(x),mm(y))

def fp_text(name, pads, outline, desc, model=None, attr='smd'):
    x0,y0,x1,y1=outline
    s=f'''(footprint {q(name)} (version 20260206) (generator pcbnew) (layer "F.Cu")
      (descr {q(desc)}) (attr {attr})
      (property "Reference" "REF**" (at 0 {y0-1} 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))
      (property "Value" {q(name)} (at 0 {y1+1} 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))
      (fp_rect (start {x0} {y0}) (end {x1} {y1}) (stroke (width 0.1) (type solid)) (fill none) (layer "F.Fab"))
      (fp_rect (start {x0-.25} {y0-.25}) (end {x1+.25} {y1+.25}) (stroke (width 0.05) (type solid)) (fill none) (layer "F.CrtYd"))
    '''
    for num,x,y,w,h,drill in pads:
        if drill:
            s+=f'(pad {q(num)} thru_hole {"rect" if str(num)=="1" else "oval"} (at {x} {y}) (size {w} {h}) (drill {drill}) (layers "*.Cu" "*.Mask"))\n'
        else:
            ffc=name.startswith('FH28') and str(num).isdigit()
            layers='"F.Cu" "F.Mask"' if ffc else '"F.Cu" "F.Paste" "F.Mask"'
            s+=f'(pad {q(num)} smd rect (at {x} {y}) (size {w} {h}) (layers {layers}))\n'
            if ffc:
                s+=f'(fp_rect (start {x-.125} -.65) (end {x+.125} .35) (stroke (width 0) (type solid)) (fill solid) (layer "F.Paste"))\n'
    if model:
        s+=f'(model "${{KIPRJMOD}}/../libraries/3dmodels/{model}" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0)))\n'
    return s+')\n'

def footprints():
    for kind,(_,_,_,name) in PARTS.items():
        if kind in ['FFC','DF23','POWER','TP']:continue
        stock='HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3x3mm' if kind=='EFUSE' else 'Converter_DCDC_TRACO_TSR-1_THT' if kind=='BUCK5' else 'SOT-23-6' if kind=='SUPERVISOR' else name
        paths=list(Path('/usr/share/kicad/footprints').glob(f'*.pretty/{stock}.kicad_mod'))
        assert len(paths)==1,(kind,name,paths)
        text=paths[0].read_text()
        if kind=='SUPERVISOR':
            text=text.replace('(footprint "'+stock+'"','(footprint "'+name+'"',1)
        if kind=='BUCK5':
            text=text.replace('(footprint "'+stock+'"','(footprint "'+name+'"',1)
            text=text.replace('${KICAD10_3DMODEL_DIR}/Converter_DCDC.3dshapes/Converter_DCDC_TRACO_TSR-1_THT.step', '${KIPRJMOD}/../libraries/3dmodels/TSR1-2450-trimmed.step')
        if kind=='BUCK33':
            text=text.replace('${KICAD10_3DMODEL_DIR}/Converter_DCDC.3dshapes/Converter_DCDC_TRACO_TSR2-xxxx_THT.step', '${KIPRJMOD}/../libraries/3dmodels/TSR2-2433-trimmed.step')
        if kind=='EFUSE':
            text=text.replace('${KICAD10_3DMODEL_DIR}/Package_SO.3dshapes/HTSSOP-16-1EP_4.4x5mm_P0.65mm_EP3x3mm.step', '${KIPRJMOD}/../libraries/3dmodels/TPS26600-PWP0016A.step')
            # TI PWP0016A drawing: 1.5x0.45 leads on 5.8 row spacing,
            # 3.3x3.3 exposed pad. Retain a windowed paste aperture.
            text=text.replace('(footprint "'+stock+'"','(footprint "'+name+'"',1)
            text=text.replace('(size 1.55 0.4)','(size 1.5 0.45)').replace('(size 3 3)','(size 3.3 3.3)')
            text=text.replace('(at -2.875 ', '(at -2.9 ').replace('(at 2.875 ', '(at 2.9 ')
            text=text.replace('(size 1.21 1.21)','(size 1.4 1.4)')
        if kind=='I2C':
            # TI DGK land drawing: 0.45 mm width leaves a 0.13 mm mask web.
            text=text.replace('(size 1.625 0.5)', '(size 1.625 0.45)')
        if kind=='AXC8':
            # The stock pin-1 triangle is inside the dense bypass capacitor area.
            # Keep it in the assembly drawing; use the explicit board pin-1 dots.
            # Change only this polygon's layer, not the entire footprint silk.
            text=re.sub(r'(\(fp_poly.*?\(layer )"F.SilkS"',r'\1"F.Fab"',text,flags=re.S)
        # Project-local models: retain the exact matching stock STEP geometry.
        for model in re.findall(r'\(model\s+"([^"]+)"',text):
            src=Path(model.replace('${KICAD10_3DMODEL_DIR}','/usr/share/kicad/3dmodels').replace('${KICAD9_3DMODEL_DIR}','/usr/share/kicad/3dmodels'))
            if src.exists():
                shutil.copyfile(src,MODELS/src.name)
                text=text.replace(model,'${KIPRJMOD}/../libraries/3dmodels/'+src.name)
        (LIB/(name+'.kicad_mod')).write_text(text)
    pads=[(i+1,19.75-i*.5,0,.3,1.3,0) for i in range(80)]
    pads += [('MP1',-22.175,-5.75,2.35,1.8,0),('MP2',22.175,-5.75,2.35,1.8,0)]
    (LIB/'FH28H-80S-0.5SH.kicad_mod').write_text(fp_text('FH28H-80S-0.5SH',pads,(-23.35,-6.65,23.35,.65),
       'Hirose FH28 catalog p5-6: A39.5 H46.7 J42.0; 0.30x1.30 pads; mouth -Y; PIN1 at +X (contact mark viewed from top)', 'FH28H-80S.step'))
    pads=[]
    for i in range(25):
        pads += [(2*i+1,-6+i*.5,-1.95,.25,1,0),(2*i+2,-6+i*.5,1.95,.25,1,0)]
    (LIB/'DF23C-50DP-0.5V.kicad_mod').write_text(fp_text('DF23C-50DP-0.5V',pads,(-7,-2.45,7,2.45),
       'Hirose EDC3-164006-03: 0.25x1.0 pads, pitch0.5, row centres3.9; Smartwin CN1 odd/even numbering mirrored for plug', 'DF23C-50DP.step'))
    (LIB/'TerminalBlock_1x02_P3.50mm.kicad_mod').write_text(fp_text('TerminalBlock_1x02_P3.50mm',[(1,0,0,2,2,1.2),(2,3.5,0,2,2,1.2)],
        (-2,-3.4,5.5,3.9),'Phoenix 1751248 MKDS 1/2-3.5; cable entry toward +Y', 'Terminal_2x3.5.step','through_hole'))
    tp=fp_text('TestPoint_1.0mm',[(1,0,0,1,1,0)],(-.5,-.5,.5,.5),'Exposed ENIG probe pad, no component',None)
    tp=tp.replace('"F.Cu" "F.Paste" "F.Mask"','"F.Cu" "F.Mask"')
    (LIB/'TestPoint_1.0mm.kicad_mod').write_text(tp)
    for p in components('giga-adapter'):
        if p['kind']!='HEADER':continue
        xs=[x['x'] for x in p['pads']];ys=[x['y'] for x in p['pads']]
        # Contact centres are copied, not snapped to a guessed Arduino grid.
        diameter=1.70 if p['ref']=='J8' else 1.8
        pads=[(v['pin'],v['x'],v['y'],diameter,diameter,1.05) for v in p['pads']]
        vertical=max(ys)-min(ys)>max(xs)-min(xs)
        ex,ey=(1.205,1.525) if vertical else (1.525,1.205)
        (LIB/(p['footprint']+'.kicad_mod')).write_text(fp_text(p['footprint'],pads,(min(xs)-ex,min(ys)-ey,max(xs)+ex,max(ys)+ey),
           f"{p['mpn']}; Arduino mating centres; socket on top; 14.83mm tails J9, 10mm others; board gap 13mm",p['footprint']+'.step','through_hole'))
    for path in LIB.glob('*.kicad_mod'):
        fp=k.FootprintLoad(str(LIB),path.stem)
        normalize_footprint(fp)
        k.FootprintSave(str(LIB),fp)

def pin_specs(part):
    kind=part['kind'];nets=part['nets']
    if kind=='AXC8':
        left=[(str(3+i),f'A{i+1}','input') for i in range(8)]+[('1','VCCA','power_in'),('2','DIR1 HIGH','input'),('11','DIR2 LOW','input'),('12','GND','power_in')]
        right=[(str(21-i),f'B{i+1}','output') for i in range(8)]+[('24','VCCB','power_in'),('23','VCCB','power_in'),('22','~{OE} LOW','input'),('13','GND','power_in')]
    elif kind=='AXC1':
        left=[('1','VCCA / PANEL','power_in'),('3','A / INTERRUPT','input'),('5','DIR HIGH','input')]
        right=[('6','VCCB / GIGA','power_in'),('4','B / INTERRUPT','output'),('2','GND','power_in')]
    elif kind=='I2C':
        left=[('1','VCCA / PANEL','power_in'),('2','SCLA','bidirectional'),('3','SDAA','bidirectional'),('4','GND','power_in')]
        right=[('8','VCCB / GIGA','power_in'),('7','SCLB','bidirectional'),('6','SDAB','bidirectional'),('5','EN','input')]
    elif kind.startswith('BUCK'):
        left=[('1','VIN','passive'),('2','GND','passive')];right=[('3','VOUT','passive')]
    elif kind=='LDO':
        left=[(str(i),n,'passive') for i,n in [(3,'FB'),(4,'GND'),(6,'SS_CTRL / GND'),(7,'EN'),(8,'NR/SS')]]
        right=[(str(i),n,'passive') for i,n in [(1,'OUT'),(2,'OUT'),(5,'PG'),(9,'IN'),(10,'IN'),(11,'EP / GND')]]
    elif kind=='SUPERVISOR':
        left=[(str(i),n,'passive') for i,n in [(3,'MR'),(4,'CT'),(5,'SENSE')]]
        right=[(str(i),n,'passive') for i,n in [(1,'RESET / READY'),(2,'GND'),(6,'VDD')]]
    elif kind=='AND4':
        left=[(str(i),n,'input') for i,n in [(1,'1A'),(2,'1B'),(4,'2A'),(5,'2B'),(9,'3A'),(10,'3B'),(12,'4A'),(13,'4B')]]
        right=[(str(i),n,'passive') for i,n in [(3,'1Y'),(6,'2Y'),(8,'3Y'),(11,'4Y'),(7,'GND'),(14,'VCC')]]
    elif kind=='EFUSE':
        left=[(str(i),n,'passive') for i,n in enumerate(['IN','IN','UVLO','NC','OVP','MODE','SHDN','RTN'],1)]
        right=[(str(i),n,'passive') for i,n in enumerate(['GND','IMON','ILIM','dVdT','NC','FLT','OUT','OUT','EP / RTN'],9)]
    elif kind=='SCHOTTKY':
        left=[('1','K','passive')];right=[('2','A','passive')]
    elif kind=='TP':
        left=[('1','PROBE','passive')];right=[]
    else:
        pins=list(nets)
        if kind in ['FFC','DF23']:
            # Physical odd/even rows on DF23; logical split on FFC for readable labels.
            left=[(i,i,'passive') for i in pins[::2]];right=[(i,i,'passive') for i in pins[1::2]]
        elif kind=='HEADER':
            half=(len(pins)+1)//2;left=[(i,i,'passive') for i in pins[:half]];right=[(i,i,'passive') for i in pins[half:]]
        else:left=[(pins[0],pins[0],'passive')];right=[(pins[1],pins[1],'passive')]
    return left,right

def symbol(part):
    name=part['kind'] if part['kind']!='HEADER' else part['footprint']
    left,right=pin_specs(part)
    small=len(part['nets'])<=2
    width=2.54 if small else 17.78
    count=max(len(left),len(right));height=max(2.54,(count+1)*1.27)
    pins={}
    body=f'(rectangle (start {-width} {height if not small else .762}) (end {width} {-height if not small else -.762}) (stroke (width .254) (type default)) (fill (type background)))'
    capacitor=part['kind'].startswith('C')
    if capacitor:
        body=''.join(f'(polyline (pts (xy {x} -1.27) (xy {x} 1.27)) (stroke (width .254) (type default)) (fill (type none)))' for x in [-.508,.508])
        width=.508
    s=f'(symbol "DisplayAdapter:{name}" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)'
    for prop,value,y in [('Reference',part['ref'][0],height+4),('Value',part['value'],height+2)]:
        s+=f'(property {q(prop)} {q(value)} (at 0 {y} 0) (effects (font (size 1.27 1.27))))'
    s+=f'(property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))'
    s+=f'(symbol "{name}_0_1" {body}) (symbol "{name}_1_1"'
    for side,items in [(-1,left),(1,right)]:
        for i,(num,label,typ) in enumerate(items):
            y=(count-1)*1.27-i*2.54
            if small:y=0
            x=side*(7.62 if capacitor else width+5.08)
            length=7.62-width if capacitor else 5.08
            s+=f'(pin {typ} line (at {x} {y} {0 if side<0 else 180}) (length {length}) (name {q(label)} (effects (font (size 1.016 1.016)))) (number {q(num)} (effects (font (size 1.016 1.016)))))'
            pins[num]=(x,-y,side)
    return name,s+'))',pins,height

def schematic(which,parts):
    dest=ROOT/which; rootid=uid(which);pages=list(dict.fromkeys(p['page'] for p in parts))
    cache={}
    for p in parts:
        name,s,_,_=symbol(p);cache[name]=s
    # Local symbol library and embedded copies are identical, so ERC can compare them.
    library='(kicad_symbol_lib (version 20250114) (generator kicad_symbol_editor)\n'
    library+='\n'.join(s.replace('"DisplayAdapter:', '"') for s in cache.values())+')\n'
    (dest/'Adapter.kicad_sym').write_text(library)
    (dest/'sym-lib-table').write_text('(sym_lib_table (lib (name "DisplayAdapter") (type "KiCad") (uri "${KIPRJMOD}/Adapter.kicad_sym") (options "") (descr "Verified local adapter symbols")))\n')
    (dest/'fp-lib-table').write_text('(fp_lib_table (lib (name "DisplayAdapter") (type "KiCad") (uri "${KIPRJMOD}/../libraries/DisplayAdapter.pretty") (options "") (descr "Project-local footprints")))\n')
    def start(sheetid,title,syms):
        return f'(kicad_sch (version 20250114) (generator eeschema) (uuid "{sheetid}") (paper "A3") (title_block (title {q(title)}) (date {q(BOARDS[which]["date"])}) (rev {q(BOARDS[which]["revision"])}) (company "ProtoMate")) (lib_symbols '+''.join(syms)+')\n'
    flag='(symbol "DisplayAdapter:SupplyPresent" (pin_names (offset 0)) (in_bom no) (on_board no) (property "Reference" "#FLG" (at 0 3 0) (effects (font (size 1 1)))) (property "Value" "SupplyPresent" (at 0 1 0) (effects (font (size 1 1)))) (symbol "SupplyPresent_0_1" (polyline (pts (xy 0 0) (xy -1.27 1.27) (xy 0 2.54) (xy 1.27 1.27) (xy 0 0)) (stroke (width .15) (type default)) (fill (type none)))) (symbol "SupplyPresent_1_1" (pin power_out line (at 0 0 90) (length 0) (name "power" (effects (font (size 1 1)))) (number "1" (effects (font (size 1 1)))))))'
    libfile=dest/'Adapter.kicad_sym';lib=libfile.read_text();libfile.write_text(lib[:-2]+flag.replace('"DisplayAdapter:', '"')+')\n')
    root=start(rootid,which+' / Smartwin display interface',[flag])
    for idx,net in enumerate(['GND','+3V3_PANEL']+(['+3V3_GIGA'] if which=='giga-adapter' else [])):
        x=30.48+idx*76.2;y=246.38;pid=uid(which+net+'flag')
        root+=f'(symbol (lib_id "DisplayAdapter:SupplyPresent") (at {x} {y} 0) (unit 1) (in_bom no) (on_board no) (dnp no) (uuid "{pid}") (property "Reference" "#FLG0{idx+1}" (at {x} {y-5} 0) (effects (font (size 1 1)) (hide yes))) (property "Value" "SupplyPresent" (at {x} {y-4} 0) (effects (font (size 1 1)))) (instances (project {q(which)} (path "/{rootid}" (reference "#FLG0{idx+1}") (unit 1)))))'
        root+=f'(global_label {q(net)} (shape input) (at {x} {y} 90) (effects (font (size 1 1)) (justify left)) (uuid "{uid(pid+"net")}") (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {x} {y} 0) (effects (font (size 1 1)) (hide yes))))'
    root+='(text "24V input on GIGA shield | Local 3.3V display conversion | <=100 mm Type-A 80-way FFC" (at 20.32 25.4 0) (effects (font (size 1.524 1.524)) (justify left)) (uuid "'+uid(which+'note')+'"))\n'
    for idx,page in enumerate(pages):
        sid=uid(which+'/'+page);filename=page+'.kicad_sch';px=20.32+(idx%3)*121.92;py=45.72+(idx//3)*60.96
        root+=f'(sheet (at {px} {py}) (size 101.6 35.56) (stroke (width .254) (type default)) (fill (color 0 0 0 0)) (uuid "{sid}") (property "Sheetname" {q(page)} (at {px} {py-1.27} 0) (effects (font (size 1.524 1.524)) (justify left))) (property "Sheetfile" {q(filename)} (at {px} {py+36.83} 0) (effects (font (size 1.27 1.27)) (justify left))) (instances (project {q(which)} (path "/{rootid}" (page "{idx+2}")))))\n'
        selected=[p for p in parts if p['page']==page]
        syms={symbol(p)[0]:symbol(p)[1] for p in selected}
        s=start(sid,which+' / '+page,list(syms.values()))
        major=[p for p in selected if len(p['nets'])>3 or p['kind'].startswith('BUCK')]
        minor=[p for p in selected if p not in major]
        poses={}
        if page=='headers':
            for i,p in enumerate(major):poses[p['ref']]=(55.88+(i%4)*101.6,60.96+(i//4)*116.84)
        else:
            for i,p in enumerate(major):poses[p['ref']]=((55.88+132.08*i if len(major)>2 else 86.36+172.72*i),91.44)
        for i,p in enumerate(minor):
            poses[p['ref']]=(45.72+(i%4)*96.52,((129.54 if page in ('power','power12') else 175.26) if major else 55.88)+(i//4)*20.32)
        for p in selected:
            name,_,pins,h=symbol(p);x,y=poses[p['ref']];pid=uid(which+'/'+p['ref']);p['uuid']=pid;p['sheet_uuid']=sid
            bom='no' if p['assembly']=='PCB' else 'yes'
            s+=f'(symbol (lib_id "DisplayAdapter:{name}") (at {x} {y} 0) (unit 1) (in_bom {bom}) (on_board yes) (dnp no) (uuid "{pid}")'
            for key,val,dy,hide in [('Reference',p['ref'],-h-4,False),('Value',p['value'],-h-2,False),('Footprint','DisplayAdapter:'+p['footprint'],0,True),('Manufacturer',p['mfr'],0,True),('MPN',p['mpn'],0,True),('Assembly',p['assembly'],0,True)]:
                size=1.016 if key=='Value' else 1.27
                s+=f'(property {q(key)} {q(val)} (at {x} {y+dy} 0) (effects (font (size {size} {size}))'+(' (hide yes)' if hide else '')+'))'
            s+=f'(instances (project {q(which)} (path "/{rootid}/{sid}" (reference {q(p["ref"])}) (unit 1)))))\n'
            for num,(dx,dy,side) in pins.items():
                a,b=x+dx,y+dy;net=p['nets'][num]
                if net is None:
                    s+=f'(no_connect (at {a} {b}) (uuid "{uid(pid+num+"nc")}"))\n';continue
                end=a+side*5.08
                s+=f'(wire (pts (xy {a} {b}) (xy {end} {b})) (stroke (width 0) (type default)) (uuid "{uid(pid+num+"wire")}"))\n'
                s+=f'(global_label {q(net)} (shape bidirectional) (at {end} {b} {0 if side<0 else 180}) (effects (font (size 1.016 1.016)) (justify {"right" if side<0 else "left"})) (uuid "{uid(pid+num+"label")}") (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {end} {b} 0) (effects (font (size 1 1)) (hide yes))))\n'
        note={'buffer1':'DIR1 = HIGH; DIR2 = LOW; OE = LOW. 33R series termination; 47k input bias. All channels A -> B.',
              'buffer2':'Same isolation and termination as U1. Both supplies must be valid for transmission.',
              'buffer3':'RGB24 physical interface. All eight green bits are connected.',
              'buffer4':'PCLK, DE, HSYNC, VSYNC, PWM, STANDBY and active-low resets. PCLK buffered by the same family as RGB.',
              'headers':'NC means no adapter trace. Socket still passes that GIGA contact straight through. Do not populate other header variants.',
              'touch':'TCA9517 A = panel; B = GIGA. GIGA R26/R27 already provide 5.1k pull-ups. AXC1 direction A -> B.',
              'ribbon':'80-way Type-A (same-side contacts), 0.5mm pitch, 0.30mm mating thickness. GIGA pin n <-> display socket pin 81-n.',
              'display':'Bottom-mounted DF23 plug. Pin orientation follows Smartwin mechanical drawing Detail A. All supply/GND pins used.',
              'logic_power':'U2: 24V -> 5V. U3: precision 3.296V, 47nF soft start (~6ms). 0.1% feedback resistors. EP must be soldered.',
              'sequencing':'Both supplies valid for 300ms before READY. Hardware gates reset, standby and PWM. READY returns to GIGA D70.',
              'power':'U1: 24V from ribbon -> BACKLIGHT 3.3V / 2A. Never join panel 3.3V to GIGA 3.3V. Module is hand soldered; trim leads.',
              'power_input':'24V nominal input. UVLO ~18.2V; OVP ~28.1V; current limit ~1.49A. RTN is NOT system GND. SHDN floats (internal pull-up).',
              'power12':'U8: 24V -> 12V / 1A. D3 blocks USB-only reverse feed into U8. F2 protects the two parallel 24V ribbon conductors.'}[page]
        s+=f'(text {q(note)} (at 20.32 22.86 0) (effects (font (size 1.27 1.27)) (justify left)) (uuid "{uid(sid+"note")}"))\n'
        (dest/filename).write_text(s+')\n')
    root+='(sheet_instances (path "/" (page "1"))))\n'
    (dest/(which+'.kicad_sch')).write_text(root)

def board(which,parts):
    b=k.BOARD();b.SetCopperLayerCount(BOARDS[which]["layers"])
    ds=b.GetDesignSettings();ds.SetBoardThickness(mm(BOARDS[which]['thickness']))
    ds.m_MinClearance=mm(.15);ds.m_TrackMinWidth=mm(.15);ds.m_MinThroughDrill=mm(BOARDS[which]['via_drill'])
    ds.m_ViasMinSize=mm(BOARDS[which]['via_diameter']);ds.m_ViasMinAnnularWidth=mm(.15)
    ds.m_SolderMaskExpansion=0;ds.m_SolderMaskMinWidth=mm(.1)
    ds.m_MinSilkTextHeight=mm(.8);ds.m_MinSilkTextThickness=mm(.15)
    nc=ds.m_NetSettings.GetDefaultNetclass();nc.SetClearance(mm(.15));nc.SetTrackWidth(mm(.18));nc.SetViaDiameter(mm(.65));nc.SetViaDrill(mm(.3))
    names=set(n for p in parts for n in p['nets'].values() if n)
    names.add('GND')
    nets={}
    for n in sorted(names):
        v=k.NETINFO_ITEM(b,n);b.Add(v);nets[n]=v
    for p in parts:
        f=k.FootprintLoad(str(LIB),p['footprint']);assert f,p['footprint']
        b.Add(f)
        f.SetReference(p['ref']);f.SetValue(p['value']);f.SetFPID(k.LIB_ID('DisplayAdapter',p['footprint']))
        f.SetPath(k.KIID_PATH('/'+uid(which)+'/'+p['sheet_uuid']+'/'+p['uuid']))
        x,y,rot,side=p['pos'];f.SetPosition(point(x,y));f.SetOrientationDegrees(rot)
        if side=='B':f.Flip(point(x,y),False)
        if p['kind']=='AXC8':
            for item in list(f.GraphicalItems()):
                if item.GetLayer()==k.F_SilkS and item.GetShape()==k.SHAPE_T_POLY:
                    item.SetLayer(k.F_Fab)
        for pad in f.Pads():
            n=p['nets'].get(pad.GetNumber())
            if pad.GetNumber().startswith('MP'): n='GND'
            if n:pad.SetNet(nets[n])
        f.Reference().SetVisible(False) # dense parts: references on assembly/Fab layer below
        f.Value().SetVisible(False)
        text=k.PCB_TEXT(b);text.SetText(p['ref']);text.SetPosition(point(x,y));text.SetTextSize(point(.8,.8));text.SetTextThickness(mm(.12));text.SetLayer(k.F_Fab if side=='F' else k.B_Fab)
        if side=='B':text.SetMirrored(True)
        if p['kind']=='HEADER':
            # Header origins are CAD coordinates; use their actual centre for annotation.
            text.SetPosition(point(sum(a['x'] for a in p['pads'])/len(p['pads']),sum(a['y'] for a in p['pads'])/len(p['pads'])))
        if p['kind'] in ['HEADER','FFC','DF23','POWER','TP']:b.Add(text)
    if which=='display-adapter':
        contour=board_outline(which);holes=[(3,12),(52,12),(3,47),(52,47)]
    else:
        contour=board_outline(which)
        holes=[p for p in GEOMETRY['mounting_holes'] if p[0]>88 or (p[0]<17 and (p[1]<5 or p[1]>49))]
    for a,c in zip(contour,contour[1:]+contour[:1]):
        line=k.PCB_SHAPE();line.SetShape(k.SHAPE_T_SEGMENT);line.SetStart(point(*a));line.SetEnd(point(*c));line.SetLayer(k.Edge_Cuts);line.SetWidth(mm(.05));b.Add(line)
    for i,(x,y) in enumerate(holes):
        f=k.FootprintLoad('/usr/share/kicad/footprints/MountingHole.pretty','MountingHole_2.2mm_M2' if which=='display-adapter' else 'MountingHole_3.2mm_M3')
        if which=='giga-adapter':
            # H1/H4 are alignment clearance holes only; original headers leave
            # insufficient room for M3 screw heads. H2/H3 accept M2 hardware.
            for item in list(f.GraphicalItems()):
                if item.GetLayer()==k.F_CrtYd:f.Remove(item)
            circle=k.PCB_SHAPE(f);circle.SetShape(k.SHAPE_T_CIRCLE);circle.SetCenter(point(0,0));circle.SetEnd(point(1.65 if y<5 else 1.85,0));circle.SetWidth(mm(.05));circle.SetLayer(k.F_CrtYd);f.Add(circle)
        f.SetReference(f'H{i+1}');f.SetPosition(point(x,y));f.Reference().SetVisible(False);f.Value().SetVisible(False);f.SetAttributes(k.FP_EXCLUDE_FROM_BOM|k.FP_EXCLUDE_FROM_POS_FILES);b.Add(f)
    # Continuous inner reference plane; display outer pours spread LDO heat.
    for layer in ([k.F_Cu,k.In1_Cu,k.B_Cu] if which=="display-adapter" else [k.In1_Cu]):
        z=k.ZONE(b);z.SetLayer(layer);z.SetNet(nets['GND']);z.SetLocalClearance(mm(.2));z.SetPadConnection(k.ZONE_CONNECTION_FULL);z.SetMinThickness(mm(.15))
        poly=z.Outline();poly.NewOutline()
        for x,y in contour:poly.Append(mm(x),mm(y))
        b.Add(z)
    for text,x,y,size in ([('SMARTWIN / '+BOARDS[which]['revision'],27.5,15,1),('24V VIA FFC',12,33,.8),('100mm TYPE-A',27.5,11,.8)] if which=='display-adapter' else [('GIGA DISPLAY / '+BOARDS[which]['revision'],51,7,1),('FFC TO DISPLAY',115,50, .8)]):
        t=k.PCB_TEXT(b);t.SetText(text);t.SetPosition(point(x,y));t.SetTextSize(point(size,size));t.SetTextThickness(mm(.15));t.SetLayer(k.F_SilkS);b.Add(t)
    out=ROOT/'build'/which;out.mkdir(parents=True,exist_ok=True)
    path=out/(which+'.kicad_pcb');k.SaveBoard(str(path),b)
    # Explicit project rules, no blanket suppression of fabrication findings.
    pro={'meta':{'filename':which+'.kicad_pro','version':1},'board':{'design_settings':{'rules':{'min_clearance':.15,'min_track_width':.15,'min_through_hole_diameter':BOARDS[which]['via_drill'],'min_via_diameter':BOARDS[which]['via_diameter'],'min_via_annular_width':.15,'min_hole_clearance':.25,'min_copper_edge_clearance':.3,'min_silk_clearance':.1,'min_silk_text_height':.8,'min_silk_text_thickness':.15}}},
         'net_settings':{'classes':[{'name':'Default','clearance':.15,'track_width':.18,'via_diameter':.65,'via_drill':.3,'microvia_diameter':.3,'microvia_drill':.1,'diff_pair_width':.18,'diff_pair_gap':.2,'diff_pair_via_gap':.25}],'meta':{'version':4}},
         'erc':{'erc_exclusions':[]}}
    # Preserve project-specific rule severities and editor settings when regenerating.
    project=ROOT/which/(which+'.kicad_pro')
    if project.exists():
        existing=json.loads(project.read_text())
        existing.setdefault('board',{}).setdefault('design_settings',{}).setdefault('rules',{}).update(pro['board']['design_settings']['rules'])
        pro=existing
    for key in ['missing_courtyard','track_not_centered_on_via','tuning_profile_track_geometries','footprint_filters_mismatch','footprint_type_mismatch']:
        pro['board']['design_settings'].setdefault('rule_severities',{})[key]='warning'
    for key in ['single_global_label','four_way_junction','simulation_model_issue','footprint_filter']:
        pro.setdefault('erc',{}).setdefault('rule_severities',{})[key]='warning'
    pro['board']['design_settings'].setdefault('drc_exclusions',[])
    (ROOT/which/(which+'.kicad_pro')).write_text(json.dumps(pro,indent=2)+'\n')
    (out/(which+'.kicad_pro')).write_text(json.dumps(pro,indent=2)+'\n')
    configure_project(which)
    configure_project(which,out)
    board_settings(b,which)
    k.SaveBoard(str(path),b)
    write_stackup(path,which)
    for table in ['fp-lib-table','sym-lib-table']:
        (out/table).write_text((ROOT/which/table).read_text().replace('${KIPRJMOD}/../libraries','${KIPRJMOD}/../../libraries').replace('${KIPRJMOD}/Adapter.kicad_sym',f'${{KIPRJMOD}}/../../{which}/Adapter.kicad_sym'))
    return b

def main():
    footprints()
    for which in ['display-adapter','giga-adapter']:
        parts=components(which);schematic(which,parts);board(which,parts)
        (ROOT/which/'components.json').write_text(json.dumps(parts,indent=2)+'\n')
    with (ROOT/'connections.csv').open('w') as f:
        w=csv.DictWriter(f,fieldnames=list(SIGNALS[0]),lineterminator="\n");w.writeheader();w.writerows(SIGNALS)
    print('Generated schematics, local footprints and build/* placement PCBs.')

if __name__=='__main__':main()
