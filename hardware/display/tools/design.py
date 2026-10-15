"""Electrical source of truth for the two display adapter designs.

Pin numbers are physical connector/device pins, not array offsets.  This file is
also consumed by the independent exported-netlist audit and manufacturing BOM.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
GEOMETRY = json.loads((ROOT/'references/giga-geometry.json').read_text())
REVISION = 'B'  # Source bundle; individual board revisions below.
BOARDS = {
    'display-adapter': dict(revision='B', date='2026-10-15', layers=2, via_drill=.25, via_diameter=.55),
    'giga-adapter': dict(revision='A', date='2026-10-14', layers=4, via_drill=.30, via_diameter=.65),
}


def release_name(board):
    return board + '-rev-' + BOARDS[board]['revision']


def copper_layers(board):
    return ['F.Cu', 'B.Cu'] if BOARDS[board]['layers'] == 2 else ['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu']

# Order follows the ribbon, not the display connector. Every signal has adjacent
# grounds. A same-contact-side FFC between facing sockets reverses socket numbering.
SIGNALS = []
for color, pins, labels, first in [
    ('R', ['PG13','PJ0','PJ1','PJ2','PJ3','PJ4','PJ5','PJ6'], list(range(23,39,2)), 12),
    ('B', ['PJ12','PG12','PJ14','PJ15','PK3','PK4','PK5','PK6'], list(range(22,38,2)), 28),
    ('G', ['PJ7','PE6','PI15','PG10','PH15','PK0','PI11','PK2'], list(range(38,54,2)), 20),
]:
    SIGNALS += [dict(name=f'{color}{i}', mcu=p, arduino=f'D{d}', panel=first+i)
                for i,(p,d) in enumerate(zip(pins, labels))]
SIGNALS += [dict(name=n,mcu=p,arduino=d,panel=c) for n,p,d,c in [
    ('PCLK','PI14','D39',41), ('DE','PK7','D41',39),
    ('HSYNC','PI10','D43',38), ('VSYNC','PI13','D45',37),
    ('PWM','PC6','D68',9), ('STANDBY','PI0','D69',43),
    ('TFT_RESET_N','PI2','D71',44), ('TOUCH_RESET_N','PI3','D72',45),
    ('TOUCH_INT_N','PB12','D74',46), ('SDA','PH12','D102',47), ('SCL','PB6','D101',48),
]]
for i,s in enumerate(SIGNALS):
    # FH28H pin 1 is on the right when the cable mouth faces up on the page.
    # Number from the far end so the RGB banks remain adjacent to their pins.
    s['giga_ffc'] = 79-2*i
    s['display_ffc'] = 81-s['giga_ffc']
    for header in ['JSIDE','J5','JDIGITAL']:
        key = {'SDA':'I2C4.SDA', 'SCL':'I2C4.SCL'}.get(s['name'],s['mcu'])
        match = next((p for p in GEOMETRY['headers'][header] if p['net']==key),None)
        if match:
            s['header'] = header
            s['header_pin'] = match['pin']
            s['adapter_header']={'JSIDE':'J8','J5':'J9','JDIGITAL':'J2'}[header]
            s['adapter_header_pin']=match['pin']
            break

def ribbon_nets(reverse=False):
    nets = {str(i):'GND' for i in range(1,81)}
    for s in SIGNALS:
        nets[str(s['giga_ffc'])] = 'LCD_'+s['name']
    for pin in (3,5,7,9):
        nets[str(pin)] = '+3V3_PANEL'
    if reverse:
        nets = {str(81-int(p)):n for p,n in nets.items()}
    nets.update(MP1='GND', MP2='GND')
    return nets

def panel_nets():
    nets={str(i):'GND' for i in range(1,51)}
    for s in SIGNALS:
        nets[str(s['panel'])]='LCD_'+s['name']
    for i in [5,6,7,8,10,49]:
        nets[str(i)]='+3V3_PANEL'
    return nets

PARTS = {
 'R33': ('33R','Yageo','RC0402FR-0733RL','R_0402_1005Metric'),
 'R47K': ('47k','Yageo','RC0402FR-0747KL','R_0402_1005Metric'),
 'R4K7': ('4k7','Yageo','RC0402FR-074K7L','R_0402_1005Metric'),
 'R100K': ('100k','Yageo','RC0402FR-07100KL','R_0402_1005Metric'),
 'C100N': ('100n / 16V X7R','Murata','GRM188R71C104KA01D','C_0603_1608Metric'),
 'C10U': ('10u / 10V X5R','Murata','GRM21BR61A106KE19L','C_0805_2012Metric'),
 'C47U': ('47u / 10V X5R','Murata','GRM32ER61A476KE20L','C_1210_3225Metric'),
 'AXC8': ('SN74AXC8T245PWR','Texas Instruments','SN74AXC8T245PWR','TSSOP-24_4.4x7.8mm_P0.65mm'),
 'AXC1': ('SN74AXC1T45DBVR','Texas Instruments','SN74AXC1T45DBVR','SOT-23-6'),
 'I2C': ('TCA9517ADGKR','Texas Instruments','TCA9517ADGKR','VSSOP-8_3x3mm_P0.65mm'),
 'PMOS': ('DMP2035U-7','Diodes Incorporated','DMP2035U-7','SOT-23'),
 'FUSE': ('2.5A fast fuse','Littelfuse','046702.5NRHF','Fuse_0603_1608Metric'),
 'POWER': ('3.3V INPUT','Phoenix Contact','1751248','TerminalBlock_1x02_P3.50mm'),
 'FFC': ('FH28H-80S-0.5SH(05)','Hirose','FH28H-80S-0.5SH(05)','FH28H-80S-0.5SH'),
 'DF23': ('DF23C-50DP-0.5V(92)','Hirose','DF23C-50DP-0.5V(92)','DF23C-50DP-0.5V'),
 'TP': ('Probe pad','PCB','PCB copper feature','TestPoint_1.0mm'),
}

def header_net(p):
    if p['net']=='GND': return 'GND'
    if p['net']=='+3V3': return '+3V3_GIGA'
    for s in SIGNALS:
        if p['net'] == {'SDA':'I2C4.SDA','SCL':'I2C4.SCL'}.get(s['name'],s['mcu']):
            return 'MCU_'+s['name']
    return None # isolated pass-through contacts, explicitly NC on our schematic

def components(which):
    out=[]
    def add(ref, kind, nets, pos, page, **extra):
        value,mfr,mpn,fp=PARTS[kind]
        out.append(dict(ref=ref,kind=kind,value=value,mfr=mfr,mpn=mpn,footprint=fp,
                        nets={str(k):v for k,v in nets.items()},pos=pos,page=page,
                        assembly='SMT',**extra))
    if which=='display-adapter':
        # Seen from the adapter top, mating onto the display's rear-view CN1:
        # pin 1 is bottom-left, pin 2 top-left (Smartwin drawing Detail A).
        add('J1','DF23',panel_nets(),(27.5,23,180,'B'),'display')
        add('J2','FFC',ribbon_nets(True),(27.5,7,0,'F'),'ribbon')
        add('J3','POWER',{1:'+3V3_INPUT',2:'GND'},(6,24,0,'F'),'power')
        out[-1]['assembly']='HAND'
        add('F1','FUSE',{1:'+3V3_INPUT',2:'+3V3_FUSED'},(13.5,26,0,'F'),'power')
        # Reverse-polarity PMOS: D to input, S to protected output; body diode
        # points from input to output. Gate pulled to return.
        add('Q1','PMOS',{1:'PWR_GATE',2:'+3V3_PANEL',3:'+3V3_FUSED'},(16,29,0,'F'),'power')
        add('Q2','PMOS',{1:'PWR_GATE',2:'+3V3_PANEL',3:'+3V3_FUSED'},(16,33,0,'F'),'power')
        add('R1','R100K',{1:'PWR_GATE',2:'GND'},(13,31,0,'F'),'power')
        for i,(x,y) in enumerate([(22,31),(26,31),(30,31),(34,31)],1):
            add(f'C{i}','C47U',{1:'+3V3_PANEL',2:'GND'},(x,y,90,'F'),'power')
        for i,(x,y) in enumerate([(18,20),(36,20)],5):
            add(f'C{i}','C10U',{1:'+3V3_PANEL',2:'GND'},(x,y,0,'F'),'power')
        for i,(x,y) in enumerate([(20,17),(36,23)],7):
            add(f'C{i}','C100N',{1:'+3V3_PANEL',2:'GND'},(x,y,0,'F'),'power')
        for i,(s,x,y) in enumerate([('PWM',39,26),('STANDBY',39,28),('TFT_RESET_N',39,30),('TOUCH_RESET_N',39,32)],2):
            add(f'R{i}','R47K',{1:'LCD_'+s,2:'GND'},(x,y,0,'F'),'power')
        for i,(s,y) in enumerate([('SDA',25),('SCL',28)],6):
            add(f'R{i}','R4K7',{1:'LCD_'+s,2:'+3V3_PANEL'},(46,y,0,'F'),'power')
        add('R8','R47K',{1:'LCD_TOUCH_INT_N',2:'+3V3_PANEL'},(46,31,0,'F'),'power')
        for i,(net,x,y) in enumerate([('+3V3_PANEL',5,18),('GND',7,18),('LCD_PCLK',38,18),('GND',40,18),('LCD_SDA',47,18),('LCD_SCL',50,18)],1):
            add(f'TP{i}','TP',{1:net},(x,y,0,'F'),'power')
            out[-1]['assembly']='PCB'
    else:
        add('J1','FFC',ribbon_nets(),(115,26.5,270,'F'),'ribbon')
        for bank in range(4):
            ys=8.5+11.5*bank
            ss=SIGNALS[bank*8:bank*8+8]
            nets={1:'+3V3_GIGA',2:'+3V3_GIGA',11:'GND',12:'GND',13:'GND',22:'GND',23:'+3V3_PANEL',24:'+3V3_PANEL'}
            for i,s in enumerate(ss):
                nets[3+i]='MCU_'+s['name'];nets[21-i]='BUF_'+s['name']
            add(f'U{bank+1}','AXC8',nets,(104,ys,0,'F'),f'buffer{bank+1}')
            for i,s in enumerate(ss):
                ri=bank*8+i+1; yy=ys+(i-3.5)*1.25
                add(f'R{ri}','R33',{1:'BUF_'+s['name'],2:'LCD_'+s['name']},(110,yy,0,'F'),f'buffer{bank+1}')
                # Weak MCU-side bias prevents floating buffer inputs before GPIO init.
                bias_pos={36:(106.5,13.25,0,'F'),54:(106.25,36.25,0,'F')}.get(ri+32,(99.2,yy,0,'F'))
                add(f'R{ri+32}','R47K',{1:'MCU_'+s['name'],2:'GND'},bias_pos,f'buffer{bank+1}')
            for k,(x,yy,net) in enumerate([(102,ys-5.35,'+3V3_GIGA'),(106,ys-5.35,'+3V3_PANEL')]):
                add(f'C{bank*2+k+1}','C100N',{1:net,2:'GND'},(x,yy,0,'F'),f'buffer{bank+1}')
        add('U5','AXC1',{1:'+3V3_PANEL',2:'GND',3:'LCD_TOUCH_INT_N',4:'MCU_TOUCH_INT_N',5:'+3V3_PANEL',6:'+3V3_GIGA'},(87,44,0,'F'),'touch')
        add('U6','I2C',{1:'+3V3_PANEL',2:'LCD_SCL',3:'LCD_SDA',4:'GND',5:'+3V3_GIGA',6:'MCU_SDA',7:'MCU_SCL',8:'+3V3_GIGA'},(35,6.2,0,'F'),'touch')
        for i,(x,y,net) in enumerate([(83.5,44,'+3V3_PANEL'),(90.5,44,'+3V3_GIGA'),(30.5,6.2,'+3V3_PANEL'),(39.5,6.2,'+3V3_GIGA')],9):
            add(f'C{i}','C100N',{1:net,2:'GND'},(x,y,90,'F'),'touch')
        add('C13','C10U',{1:'+3V3_PANEL',2:'GND'},(86,47,0,'F'),'touch')
        add('C14','C10U',{1:'+3V3_GIGA',2:'GND'},(90,47,0,'F'),'touch')
        add('R65','R47K',{1:'MCU_TOUCH_INT_N',2:'+3V3_GIGA'},(87,40,0,'F'),'touch')
        for i,(net,x,y) in enumerate([('+3V3_GIGA',101,49),('+3V3_PANEL',104,49),('GND',107,49),('LCD_PCLK',112,37),('GND',112,39)],1):
            add(f'TP{i}','TP',{1:net},(x,y,0,'F'),'touch')
            out[-1]['assembly']='PCB'
        # GIGA already has R26/R27 = 5.1k on I2C4; no duplicate host pull-ups.
        groups=[('JDIGITAL',1,10,'J2'),('JDIGITAL',11,18,'J3'),('JDIGITAL',19,26,'J4'),
                ('JANALOG',1,8,'J5'),('JANALOG',9,16,'J6'),('JANALOG',17,24,'J7'),
                ('JSIDE',1,36,'J8'),('J5',1,24,'J9')]
        for src,first,last,ref in groups:
            ps=[p for p in GEOMETRY['headers'][src] if first<=p['pin']<=last]
            rows=2 if src in ('JSIDE','J5') else 1
            qty=len(ps)//rows
            out.append(dict(ref=ref,kind='HEADER',value=f'{src} {first}..{last}',mfr='Samtec',
                        # J5 sits lower on GIGA. Its 14.83mm tail reaches through
                        # the existing 1.1mm entry holes with a 13mm board gap.
                        mpn=f'SSQ-1{qty:02d}-'+('04' if ref=='J9' else '03')+'-G-'+('D' if rows==2 else 'S'),
                        footprint=f'GIGA_{ref}_{src}',nets={str(i+1):header_net(p) for i,p in enumerate(ps)},
                        pos=(0,0,0,'F'),page='headers',assembly='HAND',
                        pads=[dict(pin=i+1,x=p['x'],y=p['y'],source_pin=p['pin'],source_net=p['net']) for i,p in enumerate(ps)]))
    return out
