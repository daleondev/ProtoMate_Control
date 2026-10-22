#!/usr/bin/env python3
"""Independent release audit of exported netlists, firmware and PCB copper.

The reference pin table below is transcribed from Smartwin pp12-14/26 and the
Arduino pinout, independently of the design generator's SIGNALS table.
"""
import csv
import hashlib
import json
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
import pcbnew as k
from design import ROOT, BOARDS
k.SwigPyIterator.next=k.SwigPyIterator.__next__

PANEL={**{n:'GND' for n in [1,2,3,4,11,36,40,42,50]},
       **{n:'+3V3_BKL' for n in [5,6,7,8]}, **{n:'+3V3_PANEL' for n in [10,49]},
       9:'PANEL_PWM',37:'LCD_VSYNC',38:'LCD_HSYNC',39:'LCD_DE',41:'LCD_PCLK',
       43:'PANEL_STANDBY',44:'PANEL_TFT_RESET_N',45:'PANEL_TOUCH_RESET_N',
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
        pcb={(f.GetReference(),p.GetNumber()):('' if p.GetNetname().startswith('unconnected-') else p.GetNetname()) for f in b.GetFootprints() for p in f.Pads()}
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
        for f in b.GetFootprints():
            for model in f.Models():
                path=model.m_Filename.replace('${KIPRJMOD}',str(ROOT/name))
                assert '${' not in path and Path(path).is_file(),(name,f.GetReference(),'missing local 3D model',path)
        for t in b.GetTracks():
            if isinstance(t,k.PCB_VIA):
                assert t.GetDrillValue()>=k.FromMM(BOARDS[name]['via_drill'])
                assert t.GetWidth(k.F_Cu)>=k.FromMM(BOARDS[name]['via_diameter'])
                assert (t.GetWidth(k.F_Cu)-t.GetDrillValue())/2>=k.FromMM(.15)
            else:
                allowed=[k.F_Cu,k.B_Cu] if BOARDS[name]['layers']==2 else [k.F_Cu,k.In2_Cu,k.B_Cu]
                assert t.GetLayer() in allowed,('Unexpected routing layer',name,t.GetNetname())
                assert t.GetWidth()>=k.FromMM(.15),(name,t.GetNetname(),t.GetWidth())
        assert b.GetCopperLayerCount()==BOARDS[name]['layers']
        assert b.GetTitleBlock().GetRevision()==BOARDS[name]['revision']
        ground_layers={z.GetLayer() for z in b.Zones() if z.GetNetname()=='GND'}
        assert k.In1_Cu in ground_layers and all(z.GetNetname()=='GND' for z in b.Zones() if z.GetLayer()==k.In1_Cu and not z.GetIsRuleArea())
        summary[name]={'revision':BOARDS[name]['revision'],'copper_layers':b.GetCopperLayerCount(),'schematic_pcb_pin_pairs':len(nets),'components':len(source_parts),'tracks':len(b.GetTracks()),'ground_reference':'Stitched F.Cu/B.Cu ground pours' if BOARDS[name]['layers']==2 else 'In1.Cu GND reference plane'}
        pins[name]=nets
    d,g=pins['display-adapter'],pins['giga-adapter']
    for p,net in PANEL.items():assert d['J1',str(p)]==net,(p,net)
    assert len(PANEL)==50
    # Every FFC conductor, including 24V feeds and the separate 3.3V reference return.
    for p in range(1,81):assert g['J1',str(p)]==d['J2',str(81-p)],p
    assert {p for p in range(1,81) if g['J1',str(p)]=='+3V3_PANEL'}=={5,7,9}
    assert sum(g['J1',str(p)]=='GND' for p in range(1,81))==39
    rows=list(csv.DictReader((ROOT/'connections.csv').open()))
    assert len(rows)==35
    ioc=next((ROOT.parents[1]/'external/CubeMX').glob('*.ioc'),None)
    reference=ROOT/'references/firmware-pins.json'
    if ioc:
        config=dict(line.strip().split('=',1) for line in ioc.read_text().splitlines() if '=' in line)
        pin_reference=dict(source=ioc.name,sha256=hashlib.sha256(ioc.read_bytes()).hexdigest(),
                           signals={key: config[key] for key in [*[p+'.Signal' for p in MCU.values()],
                               'PI1.Signal', 'PI1.PinAttribute', 'RCC.LTDCFreq_Value',
                               'LTDC.TotalWidth', 'LTDC.TotalHeigh']})
        reference.write_text(json.dumps(pin_reference,indent=2)+'\n')
        firmware_source='current repository CubeMX project'
    else:
        # Portable hardware source bundle: check the release's recorded assignments.
        pin_reference=json.loads(reference.read_text());config=pin_reference['signals']
        firmware_source='release CubeMX pin snapshot (no live firmware tree present)'
    expected={'PCLK':'LTDC_CLK','DE':'LTDC_DE','HSYNC':'LTDC_HSYNC','VSYNC':'LTDC_VSYNC',
              'PWM':'S_TIM8_CH1','STANDBY':'GPIO_Output','TFT_RESET_N':'GPIO_Output',
              'TOUCH_RESET_N':'GPIO_Output','TOUCH_INT_N':'GPXTI12','SDA':'I2C4_SDA','SCL':'I2C4_SCL'}
    for row in rows:
        n=row['name'];assert MCU[n]==row['mcu'];assert row['mcu'] not in RESERVED
        assert g[row['adapter_header'],row['adapter_header_pin']]=='MCU_'+n
        assert g['J1',row['giga_ffc']]==d['J2',row['display_ffc']]=='LCD_'+n
        assert d['J1',row['panel']]==('PANEL_' if n in ['PWM','STANDBY','TFT_RESET_N','TOUCH_RESET_N'] else 'LCD_')+n
        function='LTDC_'+n if len(n)==2 and n[0] in 'RGB' else expected[n]
        assert config[row['mcu']+'.Signal']==function,(n,config[row['mcu']+'.Signal'],function)
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
    assert {p for p in range(1,81) if g['J1',str(p)]=='+24V_DISPLAY'}=={1,3}
    assert d['U1','1']==d['C9','1']=='+24V_DISPLAY'
    assert d['U1','2']=='GND' and d['U1','3']=='+3V3_BKL'
    # Supply supervision and independent rail domains, checked pin by pin.
    assert d['U2','1']=='+24V_DISPLAY' and d['U2','3']=='+5V_LOGIC'
    assert d['U3','1']==d['U3','2']=='+3V3_PANEL'
    assert d['U3','9']==d['U3','10']==d['U3','7']=='+5V_LOGIC'
    assert d['U3','4']==d['U3','6']==d['U3','11']=='GND'
    assert d['U3','3']==d['R9','2']==d['R10','1']=='LOGIC_FB'
    assert d['R9','1']=='+3V3_PANEL' and d['R10','2']=='GND'
    assert d['U3','8']==d['C15','1']=='LOGIC_SS' and d['C15','2']=='GND'
    for ref,supply in [('U4','+3V3_PANEL'),('U6','+3V3_BKL')]:
        assert d[ref,'1']=='PANEL_READY' and d[ref,'2']=='GND' and d[ref,'5']==supply
        assert d[ref,'3']==d[ref,'6']=='+3V3_PANEL'
    assert d['R11','1']=='+3V3_PANEL' and d['R11','2']=='PANEL_READY'
    for signal,a,b,y in [('PWM',1,2,3),('STANDBY',4,5,6),('TFT_RESET_N',9,10,8),('TOUCH_RESET_N',12,13,11)]:
        assert d['U5',str(a)]=='LCD_'+signal and d['U5',str(b)]=='PANEL_READY'
        assert d['U5',str(y)]=='PANEL_'+signal
    assert d['U5','7']=='GND' and d['U5','14']=='+3V3_PANEL'
    assert d['J2','71']==g['J1','10']==g['U9','3']=='PANEL_READY'
    assert g['U9','1']==g['U9','5']=='+3V3_PANEL' and g['U9','6']=='+3V3_GIGA'
    assert g['U9','4']==g['R71','1']==g['J9','15']=='MCU_PANEL_READY'
    assert g['R71','2']=='GND'
    assert config['PI1.Signal']=='GPIO_Input' and config['PI1.PinAttribute']=='CortexM7'
    assert 'PI1' not in RESERVED
    assert config['RCC.LTDCFreq_Value']=='40960000'
    assert config['LTDC.TotalWidth']=='1119' and config['LTDC.TotalHeigh']=='609'
    assert g['J10','1']==g['F1','1']=='+24V_INPUT'
    assert g['J10','2']==g['J10','3']=='GND'
    assert g['F1','2']==g['D1','1']==g['U7','1']==g['U7','2']=='+24V_FUSED'
    assert g['U7','15']==g['U7','16']==g['U8','1']==g['F2','1']=='+24V_PROTECTED'
    assert g['F2','2']=='+24V_DISPLAY'
    assert g['U7','6']==g['U7','8']==g['U7','17']=='PWR_RTN'
    assert g['U7','9']=='GND' and g['U8','2']=='GND'
    assert g['U8','3']==g['D3','2']=='+12V_CONVERTER'
    assert g['D3','1']==g['J5','8']==g['J9','24']=='+12V_GIGA'
    for pin in ['4','7','10','13','14']:assert g['U7',pin]==''
    for ref in ['R67','R69','R70','C16']:assert g[ref,'2']=='PWR_RTN'
    assert g['D2','1']=='+24V_PROTECTED' and g['D2','2']=='GND'
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
    summary.update(display_contacts=50,ribbon_conductors=80,display_signals=35,supply_ready_signals=1,firmware_pin_conflicts=0,
                   firmware_assignment_source=firmware_source,firmware_source_sha256=pin_reference['sha256'],
                   physical_hardware_test='NOT PERFORMED')
    (reports/'electrical-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
