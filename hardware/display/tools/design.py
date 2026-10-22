"""Electrical source of truth for the two display adapter designs.

Pin numbers are physical connector/device pins, not array offsets.  This file is
also consumed by the independent exported-netlist audit and manufacturing BOM.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
GEOMETRY = json.loads((ROOT/'references/giga-geometry.json').read_text())
REVISION = 'G'  # Source bundle; individual board revisions below.
BOARDS = {
    'display-adapter': dict(revision='G', date='2026-10-22', layers=4, via_drill=.25, via_diameter=.55,
                            thickness=1.6, template='aisler-4-layer-hd-drc'),
    'giga-adapter': dict(revision='F', date='2026-10-22', layers=4, via_drill=.30, via_diameter=.65,
                         thickness=1.6, template='aisler-4-layer-hd-drc'),
}


def board_outline(board):
    if board == 'display-adapter':
        return [(0,0),(55,0),(55,50),(0,50)]
    # Inward power tab, leaving USB, SWD and the lower service opening accessible.
    return [(12,0),(122,0),(122,53.34),(11,53.34),(11,47),
            (78,47),(78,29),(42,29),(42,10),(12,10)]


def release_name(board):
    return board + '-rev-' + BOARDS[board]['revision']


def copper_layers(board):
    return ['F.Cu', 'B.Cu'] if BOARDS[board]['layers'] == 2 else ['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu']

# Order follows the ribbon, not the display connector. Every RGB pair has adjacent
# ground returns; the clock has grounds on both sides. A same-contact-side FFC between facing sockets reverses socket numbering.
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
# Molex 15018-0563 / 505110-6091, project pin 1 at +X with mouth -Y.
# Type A between facing, top-mounted sockets maps n to 61-n.
RIBBON = ['+24V_DISPLAY']*2 + ['GND']*2 + ['+3V3_PANEL']*3 + [
    'GND', 'PANEL_READY', 'LCD_SCL', 'LCD_SDA', 'GND',
    'LCD_TOUCH_INT_N', 'LCD_TOUCH_RESET_N', 'LCD_TFT_RESET_N',
    'LCD_STANDBY', 'LCD_PWM', 'GND', 'LCD_VSYNC', 'LCD_HSYNC',
    'LCD_DE', 'GND', 'LCD_PCLK', 'GND']
for colour in 'GBR':
    for high in (7,5,3,1):
        RIBBON += [f'LCD_{colour}{high}', f'LCD_{colour}{high-1}', 'GND']
assert len(RIBBON) == 60 and RIBBON.count('GND') == 19
for s in SIGNALS:
    s['giga_ffc'] = RIBBON.index('LCD_'+s['name'])+1
    s['display_ffc'] = 61-s['giga_ffc']
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
    nets = {str(i):net for i,net in enumerate(RIBBON,1)}
    if reverse:
        nets = {str(61-int(p)):n for p,n in nets.items()}
    nets.update(MP1='GND', MP2='GND')
    return nets

def panel_nets():
    nets={str(i):'GND' for i in range(1,51)}
    for s in SIGNALS:
        nets[str(s['panel'])]='LCD_'+s['name']
    for i in [10,49]:
        nets[str(i)]='+3V3_PANEL'
    for i in [5,6,7,8]:
        nets[str(i)]='+3V3_BKL'
    for signal in SIGNALS[28:32]:
        nets[str(signal['panel'])]='PANEL_'+signal['name']
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
 'BUCK12': ('TSR 1-24120 / 12V 1A','TRACO Power','TSR 1-24120','Converter_DCDC_TRACO_TSR-1_THT'),
 'BUCK33': ('TSR 2-2433 / 3.3V 2A','TRACO Power','TSR 2-2433','Converter_DCDC_TRACO_TSR2-xxxx_THT'),
 'BUCK5': ('TSR 1-2450 / 5V 1A','TRACO Power','TSR 1-2450','Converter_DCDC_TRACO_TSR-1_THT_Trimmed'),
 'LDO': ('TPS7A9101DSKR','Texas Instruments','TPS7A9101DSKR','WSON-10-1EP_2.5x2.5mm_P0.5mm_EP1.2x2mm'),
 'SUPERVISOR': ('TPS3808G33DBVR','Texas Instruments','TPS3808G33DBVR','SOT-23-6_UpperPin1'),
 'AND4': ('SN74LVC08APWR','Texas Instruments','SN74LVC08APWR','TSSOP-14_4.4x5mm_P0.65mm'),
 'R31K2': ('31.2k / 0.1%','Yageo','RT0603BRD0731K2L','R_0603_1608Metric'),
 'R10K01': ('10k / 0.1%','Yageo','RT0603BRD0710KL','R_0603_1608Metric'),
 'EFUSE': ('TPS26600PWPR','Texas Instruments','TPS26600PWPR','HTSSOP-16_PWP0016A_EP3.3x3.3mm'),
 'TVS': ('33V bidirectional TVS','Littelfuse','SMBJ33CA','D_SMB'),
 'SCHOTTKY': ('60V / 3A Schottky','Diodes Incorporated','B360A-13-F','D_SMA'),
 'FUSE2A': ('2A / 65V time-lag','Bourns','SF-1206SA200M-2','Fuse_Bourns_SF-1206SA-M'),
 'FUSE800M': ('0.8A / 63V time-lag','Bourns','SF-1206S080-2','Fuse_1206_3216Metric'),
 'POWER': ('DC-005 / 24V centre+','Sourcing Map','DC-005 / B07J4Z8B2T','BarrelJack_Horizontal_AISLER'),
 'C100N50': ('100n / 50V X7R','Murata','GRM188R71H104KA93D','C_0603_1608Metric'),
 'C1U100': ('1u / 100V X7R','Murata','GRM31CR72A105KA01L','C_1206_3216Metric'),
 'C10U50': ('10u / 50V X7R','Murata','GRM32ER71H106KA12L','C_1210_3225Metric'),
 'C10U25': ('10u / 25V X5R','Murata','GRM21BR61E106KA73L','C_0805_2012Metric'),
 'C47N': ('47n / 16V X7R','Murata','GRM188R71C473KA01D','C_0603_1608Metric'),
 'R143K': ('143k / 1%','Yageo','RC0603FR-07143KL','R_0603_1608Metric'),
 'R226K': ('226k / 1%','Yageo','RC0603FR-07226KL','R_0603_1608Metric'),
 'R10K': ('10k / 1%','Yageo','RC0603FR-0710KL','R_0603_1608Metric'),
 'R8K06': ('8.06k / 1%','Yageo','RC0603FR-078K06L','R_0603_1608Metric'),
 'FFC': ('505110-6091','Molex','505110-6091','Molex_505110-6091_1x60_P0.50mm'),
 'DF23': ('DF23C-50DP-0.5V(92)','Hirose','DF23C-50DP-0.5V(92)','DF23C-50DP-0.5V'),
 'TP': ('Probe pad','PCB','PCB copper feature','TestPoint_1.0mm'),
}

def header_net(p):
    if p['net']=='GND': return 'GND'
    if p['net']=='+3V3': return '+3V3_GIGA'
    if p['net']=='VIN': return '+12V_GIGA'
    if p['net']=='PI1': return 'MCU_PANEL_READY'
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
        add('U1','BUCK33',{1:'+24V_DISPLAY',2:'GND',3:'+3V3_BKL'},(10,26,180,'F'),'power')
        out[-1]['assembly']='HAND'
        add('C9','C10U50',{1:'+24V_DISPLAY',2:'GND'},(17,29,90,'F'),'power')
        add('C10','C100N50',{1:'+24V_DISPLAY',2:'GND'},(17,25,0,'F'),'power')
        for i,(x,y) in enumerate([(22,31),(26,31),(30,31),(34,31)],1):
            add(f'C{i}','C47U',{1:'+3V3_BKL',2:'GND'},(x,y,90,'F'),'power')
        for i,(x,y) in enumerate([(18,20),(36,20)],5):
            add(f'C{i}','C10U',{1:'+3V3_PANEL',2:'GND'},(x,y,0,'F'),'power')
        for i,(x,y) in enumerate([(20,17),(36,23)],7):
            add(f'C{i}','C100N',{1:'+3V3_PANEL',2:'GND'},(x,y,0,'F'),'power')
        for i,(s,x,y) in enumerate([('PWM',38,29),('STANDBY',38,31),('TFT_RESET_N',38,33),('TOUCH_RESET_N',38,35)],2):
            add(f'R{i}','R47K',{1:'LCD_'+s,2:'GND'},(x,y,0,'F'),'power')
        for i,(s,y) in enumerate([('SDA',25),('SCL',28)],6):
            add(f'R{i}','R4K7',{1:'LCD_'+s,2:'+3V3_PANEL'},(46,y,0,'F'),'power')
        add('R8','R47K',{1:'LCD_TOUCH_INT_N',2:'+3V3_PANEL'},(51,37,0,'F'),'power')
        for i,(net,x,y) in enumerate([('+3V3_PANEL',4.5,18),('GND',4.5,20),('LCD_PCLK',38,18),('GND',40,18),('LCD_SDA',47,18),('LCD_SCL',50,18)],1):
            add(f'TP{i}','TP',{1:net},(x,y,0,'F'),'power')
            out[-1]['assembly']='PCB'
        # The backlight converter no longer supplies the LCD/touch/interface rail.
        add('U2','BUCK5',{1:'+24V_DISPLAY',2:'GND',3:'+5V_LOGIC'},(15,38,180,'F'),'logic_power')
        out[-1]['assembly']='HAND'
        add('U3','LDO',{1:'+3V3_PANEL',2:'+3V3_PANEL',3:'LOGIC_FB',4:'GND',5:None,
            6:'GND',7:'+5V_LOGIC',8:'LOGIC_SS',9:'+5V_LOGIC',10:'+5V_LOGIC',11:'GND'},
            (27,41,180,'F'),'logic_power')
        for ref,kind,nets,pos in [
            ('C11','C10U50',{1:'+24V_DISPLAY',2:'GND'},(21,46,90,'F')),
            ('C12','C47U',{1:'+5V_LOGIC',2:'GND'},(21,40,90,'F')),
            ('C13','C47U',{1:'+3V3_PANEL',2:'GND'},(31,36.5,90,'F')),
            ('C14','C47U',{1:'+3V3_PANEL',2:'GND'},(31,45,270,'F')),
            ('C15','C47N',{1:'LOGIC_SS',2:'GND'},(25,37.5,0,'F')),
            ('R9','R31K2',{1:'+3V3_PANEL',2:'LOGIC_FB'},(31,40,0,'F')),
            ('R10','R10K01',{1:'LOGIC_FB',2:'GND'},(34,41.5,0,'F')),
            ('R11','R10K',{1:'+3V3_PANEL',2:'PANEL_READY'},(39,46,0,'F')),
        ]: add(ref,kind,nets,pos,'logic_power')
        # Both rails must be valid for 300 ms. Open-drain resets are wired AND.
        for ref,x,sense,cap,res in [('U4',38,'+3V3_PANEL','C16','R16'),('U6',47,'+3V3_BKL','C18','R17')]:
            add(ref,'SUPERVISOR',{1:'PANEL_READY',2:'GND',3:'+3V3_PANEL',4:ref+'_DELAY',5:sense,6:'+3V3_PANEL'},(x,41,0,'F'),'sequencing')
            add(cap,'C100N',{1:'+3V3_PANEL',2:'GND'},(x,37.5,0,'F'),'sequencing')
            add(res,'R100K',{1:ref+'_DELAY',2:'+3V3_PANEL'},(x,44.5,0,'F'),'sequencing')
        add('U5','AND4',{1:'LCD_PWM',2:'PANEL_READY',3:'PANEL_PWM',4:'LCD_STANDBY',5:'PANEL_READY',6:'PANEL_STANDBY',7:'GND',
            8:'PANEL_TFT_RESET_N',9:'LCD_TFT_RESET_N',10:'PANEL_READY',11:'PANEL_TOUCH_RESET_N',12:'LCD_TOUCH_RESET_N',13:'PANEL_READY',14:'+3V3_PANEL'},
            (44,32,0,'F'),'sequencing')
        add('C17','C100N',{1:'+3V3_PANEL',2:'GND'},(42,27,0,'F'),'sequencing')
        for i,(name,y) in enumerate([('PWM',29),('STANDBY',31),('TFT_RESET_N',33),('TOUCH_RESET_N',35)],12):
            add(f'R{i}','R10K',{1:'PANEL_'+name,2:'GND'},(51,y,0,'F'),'sequencing')
        add('TP7','TP',{1:'PANEL_READY'},(42,46,0,'F'),'sequencing');out[-1]['assembly']='PCB'
        add('TP8','TP',{1:'+3V3_BKL'},(18,34,0,'F'),'power');out[-1]['assembly']='PCB'
        add('TP9','TP',{1:'+5V_LOGIC'},(4,39,0,'F'),'logic_power');out[-1]['assembly']='PCB'
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
        # Centre positive; sleeve and its normally closed switch are both GND.
        # Plug detection is unused. Mouth faces left into the service opening.
        add('J10','POWER',{1:'+24V_INPUT',2:'GND',3:'GND'},(53,23,0,'F'),'power_input')
        out[-1]['assembly']='HAND'
        add('F1','FUSE2A',{1:'+24V_INPUT',2:'+24V_FUSED'},(59,21.1,0,'F'),'power_input')
        add('D1','TVS',{1:'+24V_FUSED',2:'GND'},(59,25,0,'F'),'power_input')
        add('C15','C1U100',{1:'+24V_FUSED',2:'GND'},(46,12,90,'F'),'power_input')
        add('U7','EFUSE',{1:'+24V_FUSED',2:'+24V_FUSED',3:'PWR_UVLO',4:None,5:'PWR_OVP',
            6:'PWR_RTN',7:None,8:'PWR_RTN',9:'GND',10:None,11:'PWR_ILIM',12:'PWR_DVDT',
            13:None,14:None,15:'+24V_PROTECTED',16:'+24V_PROTECTED',17:'PWR_RTN'},
            (53,15.2,0,'F'),'power_input')
        for ref,kind,nets,pos in [
            ('R66','R143K',{1:'+24V_FUSED',2:'PWR_UVLO'},(46,15.4,0,'F')),
            ('R67','R10K',{1:'PWR_UVLO',2:'PWR_RTN'},(46,17.2,0,'F')),
            ('R68','R226K',{1:'+24V_FUSED',2:'PWR_OVP'},(49,10,0,'F')),
            ('R69','R10K',{1:'PWR_OVP',2:'PWR_RTN'},(53,10,0,'F')),
            ('R70','R8K06',{1:'PWR_ILIM',2:'PWR_RTN'},(59,18,0,'F')),
        ]: add(ref,kind,nets,pos,'power_input')
        add('C16','C47N',{1:'PWR_DVDT',2:'PWR_RTN'},(59,16,0,'F'),'power_input')
        add('C17','C10U50',{1:'+24V_PROTECTED',2:'GND'},(59.5,11,90,'F'),'power_input')
        add('D2','SCHOTTKY',{1:'+24V_PROTECTED',2:'GND'},(65,23.5,90,'F'),'power_input')
        add('F2','FUSE800M',{1:'+24V_PROTECTED',2:'+24V_DISPLAY'},(71,25.5,0,'F'),'power12')
        add('U8','BUCK12',{1:'+24V_PROTECTED',2:'GND',3:'+12V_CONVERTER'},(67,12,0,'F'),'power12')
        out[-1]['assembly']='HAND'
        add('C18','C10U50',{1:'+24V_PROTECTED',2:'GND'},(65,17.5,90,'F'),'power12')
        add('C19','C10U25',{1:'+12V_CONVERTER',2:'GND'},(75.5,16.5,90,'F'),'power12')
        add('D3','SCHOTTKY',{1:'+12V_GIGA',2:'+12V_CONVERTER'},(71,19.5,0,'F'),'power12')
        add('TP6','TP',{1:'+24V_PROTECTED'},(74,23.5,0,'F'),'power12');out[-1]['assembly']='PCB'
        add('TP7','TP',{1:'+12V_GIGA'},(69,16,0,'F'),'power12');out[-1]['assembly']='PCB'
        add('U9','AXC1',{1:'+3V3_PANEL',2:'GND',3:'PANEL_READY',4:'MCU_PANEL_READY',5:'+3V3_PANEL',6:'+3V3_GIGA'},(88.2,34,0,'F'),'touch')
        add('C20','C100N',{1:'+3V3_PANEL',2:'GND'},(88,30,0,'F'),'touch')
        add('C21','C100N',{1:'+3V3_GIGA',2:'GND'},(90,27.5,0,'F'),'touch')
        add('R71','R10K',{1:'MCU_PANEL_READY',2:'GND'},(88,38,0,'F'),'touch')
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
