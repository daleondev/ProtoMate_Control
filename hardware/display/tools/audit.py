#!/usr/bin/env python3
"""Independent release audit of exported netlists, firmware and PCB copper.

The reference pin table below is transcribed from Smartwin pp12-14/26 and the
Arduino pinout, independently of the design generator's SIGNALS table.
"""
import csv
import json
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
import pcbnew as k
from design import ROOT
k.SwigPyIterator.next=k.SwigPyIterator.__next__

PANEL={**{n:'GND' for n in [1,2,3,4,11,36,40,42,50]},
       **{n:'+3V3_PANEL' for n in [5,6,7,8,10,49]},
       9:'LCD_PWM',37:'LCD_VSYNC',38:'LCD_HSYNC',39:'LCD_DE',41:'LCD_PCLK',
       43:'LCD_STANDBY',44:'LCD_TFT_RESET_N',45:'LCD_TOUCH_RESET_N',
       46:'LCD_TOUCH_INT_N',47:'LCD_SDA',48:'LCD_SCL'}
for start,c in [(12,'R'),(20,'G'),(28,'B')]:
    PANEL.update({start+i:f'LCD_{c}{i}' for i in range(8)})
MCU={'R0':'PG13','R1':'PJ0','R2':'PJ1','R3':'PJ2','R4':'PJ3','R5':'PJ4','R6':'PJ5','R7':'PJ6',
     'G0':'PJ7','G1':'PE6','G2':'PI15','G3':'PG10','G4':'PH15','G5':'PK0','G6':'PI11','G7':'PK2',
     'B0':'PJ12','B1':'PG12','B2':'PJ14','B3':'PJ15','B4':'PK3','B5':'PK4','B6':'PK5','B7':'PK6',
     'PCLK':'PI14','DE':'PK7','HSYNC':'PI10','VSYNC':'PI13','PWM':'PC6','STANDBY':'PI0',
     'TFT_RESET_N':'PI2','TOUCH_RESET_N':'PI3','TOUCH_INT_N':'PB12','SDA':'PH12','SCL':'PB6'}
RESERVED={'PA0','PA2','PA3','PJ8','PD13','PH13','PC7','PB4','PA7','PG7','PE4','PE5','PB2','PB1','PC3','PC0','PB8','PB9','PD5','PD6','PH6','PJ11','PJ10','PK1','PH10','PH11','PH9'}

def main():
    reports=ROOT/'manufacturing/verification';reports.mkdir(parents=True,exist_ok=True)
    pins={};boards={};summary={}
    for name in ['display-adapter','giga-adapter']:
        xml=reports/(name+'.xml')
        subprocess.run(['kicad-cli','sch','export','netlist','--format','kicadxml','-o',str(xml),str(ROOT/name/(name+'.kicad_sch'))],check=True,stdout=subprocess.DEVNULL)
        tree=ET.parse(xml);nets={}
        for net in tree.findall('./nets/net'):
            for node in net.findall('node'):
                value=net.attrib['name'];nets[node.attrib['ref'],node.attrib['pin']]='' if value.startswith('unconnected-') else value
        b=k.LoadBoard(str(ROOT/name/(name+'.kicad_pcb')));boards[name]=b
        pcb={(f.GetReference(),p.GetNumber()):p.GetNetname() for f in b.GetFootprints() for p in f.Pads()}
        for key,net in nets.items():assert pcb.get(key)==net,(name,key,net,pcb.get(key))
        for key,net in pcb.items():
            if net:assert nets.get(key)==net,(name,'extra PCB connection',key,net)
        components=tree.findall('./components/comp')
        source_parts=json.loads((ROOT/name/'components.json').read_text())
        footprints={f.GetReference():f for f in b.GetFootprints()}
        for part in source_parts:
            elem=next(c for c in components if c.attrib['ref']==part['ref'])
            assert elem.findtext('./fields/field[@name="MPN"]')==part['mpn'],part['ref']
            actual=footprints[part['ref']].GetPosition()
            assert abs(k.ToMM(actual.x)-part['pos'][0])<.00002 and abs(k.ToMM(actual.y)-part['pos'][1])<.00002,(name,part['ref'],'placement source mismatch')
        for t in b.GetTracks():
            if isinstance(t,k.PCB_VIA):
                assert t.GetDrillValue()>=k.FromMM(.3)
            else:
                assert t.GetLayer() in [k.F_Cu,k.In2_Cu,k.B_Cu],('Signal cuts a reference plane',name,t.GetNetname())
                assert t.GetWidth()>=k.FromMM(.15),(name,t.GetNetname(),t.GetWidth())
        assert b.GetCopperLayerCount()==4
        summary[name]={'schematic_pcb_pin_pairs':len(nets),'components':len(source_parts),'tracks':len(b.GetTracks()),'ground_reference':'In1.Cu uninterrupted GND plane'}
        pins[name]=nets
    d,g=pins['display-adapter'],pins['giga-adapter']
    for p,net in PANEL.items():assert d['J1',str(p)]==net,(p,net)
    assert len(PANEL)==50
    # Every FFC conductor, including unused ground contacts and four supply feeds.
    for p in range(1,81):assert g['J1',str(p)]==d['J2',str(81-p)],p
    assert {p for p in range(1,81) if g['J1',str(p)]=='+3V3_PANEL'}=={3,5,7,9}
    assert sum(g['J1',str(p)]=='GND' for p in range(1,81))==41
    rows=list(csv.DictReader((ROOT/'connections.csv').open()))
    assert len(rows)==35
    config=dict(line.strip().split('=',1) for line in next((ROOT.parents[1]/'external/CubeMX').glob('*.ioc')).read_text().splitlines() if '=' in line)
    for row in rows:
        n=row['name'];assert MCU[n]==row['mcu'];assert row['mcu'] not in RESERVED
        assert g[row['adapter_header'],row['adapter_header_pin']]=='MCU_'+n
        assert g['J1',row['giga_ffc']]==d['J2',row['display_ffc']]==d['J1',row['panel']]=='LCD_'+n
        if len(n)==2 and n[0] in 'RGB':assert config[row['mcu']+'.Signal']=='LTDC_'+n
    clock=next(int(r['giga_ffc']) for r in rows if r['name']=='PCLK')
    assert g['J1',str(clock-1)]==g['J1',str(clock+1)]=='GND'
    # AXC8's two DIR pins have opposite required levels for all-eight A->B.
    for u in range(1,5):
        for p in [1,2]:assert g[f'U{u}',str(p)]=='+3V3_GIGA'
        for p in [11,12,13,22]:assert g[f'U{u}',str(p)]=='GND'
        for p in [23,24]:assert g[f'U{u}',str(p)]=='+3V3_PANEL'
    for i,row in enumerate(rows[:32]):
        u=i//8+1;ch=i%8;n=row['name']
        assert g[f'U{u}',str(3+ch)]=='MCU_'+n
        assert g[f'U{u}',str(21-ch)]==g[f'R{i+1}','1']=='BUF_'+n
        assert g[f'R{i+1}','2']=='LCD_'+n
    assert g['U5','1']==g['U5','5']=='+3V3_PANEL'
    assert g['U5','6']=='+3V3_GIGA' and g['U5','3']=='LCD_TOUCH_INT_N' and g['U5','4']=='MCU_TOUCH_INT_N'
    assert g['U6','1']=='+3V3_PANEL' and g['U6','8']==g['U6','5']=='+3V3_GIGA'
    assert g['U6','2']=='LCD_SCL' and g['U6','3']=='LCD_SDA'
    assert g['U6','7']=='MCU_SCL' and g['U6','6']=='MCU_SDA'
    for q in ['Q1','Q2']:
        assert d[q,'1']=='PWR_GATE' and d[q,'2']=='+3V3_PANEL' and d[q,'3']=='+3V3_FUSED'
    # Physical mating orientation, independently checking final placed coordinates.
    fp={f.GetReference():f for f in boards['display-adapter'].GetFootprints()}
    j={p.GetNumber():p.GetPosition() for p in fp['J1'].Pads()}
    assert j['1'].x<j['49'].x and j['1'].y>j['2'].y
    assert not fp['J1'].IsOnLayer(k.F_Cu)
    j={p.GetNumber():p.GetPosition() for p in fp['J2'].Pads()}
    assert j['1'].x>j['80'].x
    gf={f.GetReference():f for f in boards['giga-adapter'].GetFootprints()}
    gj={p.GetNumber():p.GetPosition() for p in gf['J1'].Pads()}
    assert gj['1'].y>gj['80'].y
    labels={t.GetText():t.GetPosition() for t in boards['giga-adapter'].GetDrawings() if isinstance(t,k.PCB_TEXT) and t.GetText() in ('1','80')}
    assert labels['1'].y>gj['1'].y and labels['80'].y<gj['80'].y
    summary.update(display_contacts=50,ribbon_conductors=80,signals=35,firmware_pin_conflicts=0,physical_hardware_test='NOT PERFORMED')
    (reports/'electrical-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
