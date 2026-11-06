from pathlib import Path
from html import escape
from PIL import ImageFont
import hashlib, json

OUT=Path(__file__).resolve().parent
ROOT=OUT.parents[1]
manifest=json.loads((ROOT/'build/giga-release/images/manifest.json').read_text())
pair=manifest['pair_id'][:16]
m4,m7=manifest['images']
fast=next(s['size'] for s in m4['segments'] if s['run']==0x10000800)
data_end=max(s['run']+s['size'] for s in m4['segments'] if 0x30030800<=s['run']<0x30046000)
data=data_end-0x30030800
W,H=1800,1320
C={'ink':'#183044','muted':'#566B7B','line':'#D7E1E9','paper':'#F5F8FB','white':'#FFFFFF','ui':'#245EA8','uil':'#EDF4FE','io':'#007D78','iol':'#EAF7F4','shared':'#A8680C','sharedl':'#FFF5DF','purple':'#7353A6','purplel':'#F4EFFA','red':'#B63B43','redl':'#FFF0F1','green':'#28724D','greenl':'#ECF7F0'}
FONT='/usr/share/fonts/TTF/DejaVuSans.ttf'
BOLD='/usr/share/fonts/TTF/DejaVuSans-Bold.ttf'
MONO='/usr/share/fonts/TTF/DejaVuSansMono.ttf'
fontcache={}
def font(size,bold=False,mono=False):
    key=(size,bold,mono)
    if key not in fontcache: fontcache[key]=ImageFont.truetype(MONO if mono else BOLD if bold else FONT,size)
    return fontcache[key]
def wrap(s,width,size=23,bold=False,mono=False):
    result=[]
    for para in s.split('\n'):
        line=''
        for word in para.split():
            trial=f'{line} {word}'.strip()
            if line and font(size,bold,mono).getlength(trial)>width:
                result.append(line);line=word
            else: line=trial
        result.append(line)
    return result
class Fig:
    def __init__(self,num,title,subtitle):
        self.num=num
        self.s=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="title desc">',f'<title id="title">{escape(title)}</title>',f'<desc id="desc">{escape(subtitle)}</desc>', '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#566B7B"/></marker></defs>']
        self.rect(0,0,W,H,C['paper'],r=0)
        self.text(60,53,'PROTOMATE  /  CURRENT IMPLEMENTATION',19,C['muted'],True)
        self.text(60,110,title,43,C['ink'],True)
        self.text(60,156,subtitle,23,C['muted'])
        self.text(1740,61,f'{num:02} / 04',22,C['muted'],True,anchor='end')
        self.line(60,183,1740,183,C['line'],2)
    def rect(self,x,y,w,h,fill,stroke=None,r=14,sw=1.5):
        self.s.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"'+(f' stroke="{stroke}" stroke-width="{sw}"' if stroke else '')+'/>')
    def text(self,x,y,t,size=23,color=None,bold=False,mono=False,anchor='start'):
        self.s.append(f'<text x="{x}" y="{y}" font-family="{("DejaVu Sans Mono" if mono else "DejaVu Sans")}" font-size="{size}" font-weight="{700 if bold else 400}" text-anchor="{anchor}" fill="{color or C["ink"]}">{escape(t)}</text>')
    def para(self,x,y,t,width,size=23,color=None,bold=False,leading=None,mono=False):
        leading=leading or size*1.40
        lines=wrap(t,width,size,bold,mono)
        for i,ln in enumerate(lines): self.text(x,y+i*leading,ln,size,color,bold,mono)
        return y+len(lines)*leading
    def line(self,x1,y1,x2,y2,color=None,sw=2,arrow=False,both=False,dash=False):
        self.s.append(f'<path d="M{x1},{y1} L{x2},{y2}" fill="none" stroke="{color or C["muted"]}" stroke-width="{sw}"'+(' marker-end="url(#arrow)"' if arrow or both else '')+(' marker-start="url(#arrow)"' if both else '')+(' stroke-dasharray="7 6"' if dash else '')+'/>')
    def path(self,d,arrow=False,color=None):
        self.s.append(f'<path d="{d}" fill="none" stroke="{color or C["muted"]}" stroke-width="2.3"'+(' marker-end="url(#arrow)"' if arrow else '')+'/>')
    def box(self,x,y,w,h,title,body='',color='ink',fill='white',ts=25,bs=21):
        self.rect(x,y,w,h,C[fill],C['line'])
        yy=self.para(x+22,y+35,title,w-44,ts,C[color],True,leading=ts*1.25)+12
        if body:self.para(x+22,yy,body,w-44,bs,C['ink'],leading=bs*1.35)
    def bar(self,x,y,w,ratio,color,hh=16):
        self.rect(x,y,w,hh,C['line'],r=hh/2)
        self.rect(x,y,w*ratio,hh,C[color],r=hh/2)
    def footer(self,sources):
        self.line(60,1260,1740,1260,C['line'],2)
        self.text(60,1288,f'Current source • Normal Release pair: {pair} • Electrical tests do not establish mechanical qualification',17,C['muted'])
        self.text(60,1310,sources,14,C['muted'])
    def save(self,name,sources,proofs):
        self.footer(sources)
        hashes={p:hashlib.sha256(Path(p if p.startswith('/') else ROOT/p).read_bytes()).hexdigest() for p in proofs if Path(p if p.startswith('/') else ROOT/p).exists()}
        self.s.append('<metadata>'+escape(json.dumps({'date':'2026-11-06','normal_release_pair':manifest['pair_id'],'evidence_sha256':hashes,'scope':'Current source and normal Release linked memory. Hardware fixture is distinguished explicitly.'}))+'</metadata></svg>')
        (OUT/(name+'.svg')).write_text('\n'.join(self.s))

# 1 — private applications and shared serialized transport
f=Fig(1,'How commands reach the motors','Two private applications, two buses and two bridges; only serialized frames cross between cores.')
f.rect(60,215,790,354,C['uil'],C['line'])
f.rect(950,215,790,354,C['iol'],C['line'])
f.text(84,255,'M7 · UI controller · 480 MHz',29,C['ui'],True)
f.text(974,255,'M4 · IO controller · 240 MHz',29,C['io'],True)
for x,t,b in [(84,'CLI + GUI','Commands and\npresentation'),(346,'Motion client','Tracks requests\nand job history'),(608,'Planning','Conversions, paths\nand profiles')]:
    f.box(x,281,218,116,t,b,'ui',ts=20,bs=17)
f.line(308,339,337,339,arrow=True)
f.line(570,339,599,339,both=True)
f.box(84,435,742,108,'Private pneumo bus ↔ ICC Bridge','One ICC coroutine context / worker (priority 4). All three lanes dispatch here.','ui',ts=26,bs=21)
f.line(455,398,455,432,both=True)
for x,t,b in [(974,'Controller','Admission and\nownership'),(1236,'Executor','Profiles → pulse\nschedules'),(1498,'TIM2 + DMA','Three physical\nSTEP outputs')]:
    f.box(x,281,218,116,t,b,'io',ts=20,bs=17)
f.line(1198,339,1227,339,arrow=True)
f.line(1460,339,1489,339,arrow=True)
f.box(974,435,742,108,'Private pneumo bus ↔ ICC Bridge','One ICC coroutine context / worker (priority 4). Execution runs in separate workers.','io',ts=26,bs=21)
f.line(1083,432,1083,399,both=True)
f.line(455,574,455,631,both=True)
f.line(1345,574,1345,631,both=True)
f.text(480,610,'encode / decode',20,C['muted'])
f.text(1370,610,'encode / decode',20,C['muted'])
f.rect(60,638,1680,303,C['white'],C['line'])
f.text(84,678,'Shared SRAM4 · three independent queues in each direction',29,C['shared'],True)
f.text(1716,677,'6 queues total',20,C['muted'],anchor='end')
lanes=[(84,'URGENT','Stop, cancel, lease and link control','Up to 2,048 bytes per frame','red','redl'),(640,'NORMAL','Preparation, actions, replies and state','Up to 2,048 bytes per frame','ui','uil'),(1196,'BACKGROUND','IO logs → UI → USB console','Up to 512 bytes per frame','shared','sharedl')]
for x,t,b,limit,col,fill in lanes:
    f.rect(x,704,520,168,C[fill],C['line'])
    f.text(x+20,741,t,24,C[col],True)
    f.text(x+20,774,b,19,C['ink'])
    f.text(x+20,803,limit,18,C['muted'])
    for i in range(4):f.rect(x+20+i*39,824,29,25,C['white'],C[col],r=4)
    f.text(x+190,844,'4 slots per direction',19,C['muted'])
f.text(84,914,'HSEM is the doorbell: it wakes a notification worker. The message bytes remain in shared memory.',23,C['ink'])
f.box(60,975,812,249,'What crosses the bridge',color='ink',ts=27)
f.text(84,1051,'Services',22,C['ink'],True);f.text(230,1051,'request → deferred reply',22)
f.text(84,1090,'Actions',22,C['ink'],True);f.text(230,1090,'acceptance → progress → terminal result',22)
f.text(84,1129,'Topics',22,C['ink'],True);f.text(230,1129,'latest motor state at 20 Hz; bounded logs',22)
f.para(84,1170,'Routes are declared in common/icc/catalog/{services,actions,topics}.hpp.',764,18,C['muted'])
f.box(908,975,832,249,'Separate budgets, shared dispatch',color='shared',fill='sharedl',ts=27)
f.para(932,1050,'Urgent traffic has its own capacity, but it still needs CPU time in the same ICC context.',784,22)
f.line(932,1113,1716,1113,C['line'])
f.text(932,1149,'UI lease: renew every 50 ms · expires after 250 ms',21)
f.text(932,1184,'M4 execution: 40 ms renewal · 80 ms commit frontier',21)
f.save('01-communication','Sources: ICC catalogs / Bridge / InterCoreTransport; hal/util/InterCoreMemory.hpp; motion/OperatingEnvelope.hpp', ['platform/hal/util/InterCoreMemory.hpp','src/common/icc/catalog/services.hpp','src/common/icc/catalog/topics.hpp','src/common/motion/OperatingEnvelope.hpp'])

# 2 — separate RAM ownership from storage, not drawn to scale
f=Fig(2,'Memory: storage, execution and working space','Logical allocations, not to scale. RAM usage below belongs to the normal Release linked image.')
for x,w,title,col,fill in [(60,520,'M4 private RAM','io','iol'),(640,520,'M7 private RAM','ui','uil'),(1220,520,'Shared / external RAM','shared','sharedl')]:
    f.rect(x,216,w,728,C[fill],C['line']);f.text(x+24,257,title,29,C[col],True)
# M4
f.text(84,301,'D2 SRAM · 288 KiB total',25,C['io'],True)
f.text(84,331,'Code alias 0x10000000 = physical 0x30000000',16,C['muted'],mono=True)
f.box(84,351,472,58,'2 KiB vectors · 0x10000000',color='io',ts=21)
f.rect(84,419,472,117,C['white'],C['line'])
f.text(104,451,'192 KiB fast code + constants',23,C['io'],True)
f.text(104,482,f'0x10000800 · {fast:,} B linked',18,mono=True)
f.bar(104,502,432,fast/196608,'io',13)
f.rect(84,547,472,99,C['white'],C['line'])
f.text(104,578,'86 KiB data + DMA buffers',23,C['io'],True)
f.text(104,607,f'0x30030800 · {data:,} B linked',18,mono=True)
f.bar(104,623,432,data/88064,'io',10)
f.box(84,658,472,57,'8 KiB interrupt stack · 0x30046000',color='io',ts=19)
f.rect(84,738,472,140,C['white'],C['line'])
f.text(104,771,'AXI heap region · 256 KiB',24,C['io'],True)
f.text(104,803,'0x24040000 → 0x24080000',18,mono=True)
f.para(104,834,'Unwind metadata precedes the heap.\nHeap capacity is checked at runtime.',432,19)
f.text(84,919,'The SRAM alias is not additional memory.',19,C['muted'])
# M7
f.rect(664,288,472,107,C['white'],C['line'])
f.text(684,320,'ITCM · 64 KiB',24,C['ui'],True)
f.text(684,351,'0x00000000 · 16,088 B linked',18,mono=True)
f.bar(684,369,432,16088/65536,'ui',10)
f.rect(664,410,472,185,C['white'],C['line'])
f.text(684,442,'DTCM · 128 KiB',24,C['ui'],True)
f.text(684,475,'0x20000000: vectors, 2 KiB',17,mono=True)
f.text(684,503,'0x20000800: data, 110 KiB',17,mono=True)
f.text(684,531,'0x2001C000: gap, 8 KiB',17,mono=True)
f.text(684,559,'0x2001E000: interrupt stack, 8 KiB',17,mono=True)
f.rect(664,612,472,143,C['white'],C['line'])
f.text(684,644,'AXI private region · 256 KiB',24,C['ui'],True)
f.text(684,676,'0x24000000 → 0x24040000',18,mono=True)
f.para(684,707,'Static data and ordinary heap.\nHeap capacity is checked at runtime.',432,19)
f.box(664,775,472,103,'Thread stacks use RAM too','The UI planner has a static DTCM stack. Other UI workers consume private RAM.','ui',ts=21,bs=18)
f.text(664,919,'An 8 KiB interrupt stack is not a thread pool.',18,C['muted'])
# Shared and external
f.rect(1244,288,472,119,C['white'],C['line'])
f.text(1264,321,'SRAM4 · 64 KiB',24,C['shared'],True)
f.text(1264,351,'0x38000000 · ICC reserve: 60 KiB',17,mono=True)
f.text(1264,378,'49,312 B used; final 4 KiB: boot handoff',18)
f.text(1244,446,'External SDRAM · 8 MiB',25,C['shared'],True)
f.rect(1244,467,472,103,C['white'],C['line'])
f.text(1264,499,'Framebuffers · 4 MiB reserved',23,C['shared'],True)
f.text(1264,530,'0xC0000000 · non-cacheable',18,mono=True)
f.text(1264,555,'Two images, 1,228,800 B each',19)
f.rect(1244,583,472,103,C['white'],C['line'])
f.text(1264,615,'M4 RAM-debug code · 2 MiB',23,C['io'],True)
f.text(1264,646,'0xC0400000 · optional debug image',18,mono=True)
f.text(1264,671,'Normal Release uses QSPI for cold code',18,C['muted'])
f.rect(1244,699,472,159,C['white'],C['line'])
f.text(1264,731,'UI working arena · 2 MiB',23,C['ui'],True)
f.text(1264,762,'0xC0600000 · cacheable',18,mono=True)
f.text(1264,793,'3D colour/depth buffers: 1,500 KiB',18)
f.text(1264,822,'Planning arena: 256 KiB',18)
f.bar(1264,837,432,1798144/2097152,'ui',9)
f.para(1244,893,'Spare SDRAM does not automatically enlarge either private heap.',472,19,C['muted'])
# persistent storage
f.rect(60,974,520,262,C['white'],C['line'])
f.text(84,1017,'Internal flash · 2 MiB',27,C['purple'],True)
f.text(84,1059,'M7 bootstrap: 0x08000000 · 128 KiB',18,mono=True)
f.text(84,1095,'M4 bootstrap: 0x08100000 · 128 KiB',18,mono=True)
f.text(84,1131,'File storage:  0x08180000 · 512 KiB',18,mono=True)
f.para(84,1180,'Bootstrap initializes memory and copies RAM sections before starting both applications.',472,20,C['muted'])
f.rect(640,974,1100,262,C['white'],C['line'])
f.text(664,1017,'External QSPI flash · 16 MiB · images survive power-off',27,C['purple'],True)
x=664; width=1052
for size,lab,co in [(1,'1','muted'),(3,'M4 · 3 MiB','io'),(4,'M7 · 4 MiB','ui'),(6,'Assets · 6 MiB','purple'),(1,'1','muted'),(1,'1','muted')]:
    ww=width*size/16;f.rect(x,1041,ww-3,44,C[co],r=4);f.text(x+(ww-3)/2,1070,lab,18,C['white'],True,anchor='middle');x+=ww
rows=[('0x90000000 · Factory area · 1 MiB','0x90800000 · Future assets · 6 MiB'),('0x90100000 · M4 image · 3 MiB','0x90E00000 · Manifest / scratch · 1 MiB'),('0x90400000 · M7 image · 4 MiB','0x90F00000 · Factory area · 1 MiB')]
for i,(a,b) in enumerate(rows):f.text(664,1121+i*36,a,18,mono=True);f.text(1200,1121+i*36,b,17,mono=True)
f.save('02-memory-layout','Sources: cmake/application_giga.cmake; boot linker/manifest; normal Release manifest. Heap capacity excludes linker/runtime metadata.', ['cmake/application_giga.cmake','platform/boot/giga/application.ld.in','build/giga-release/images/manifest.json'])

# 3 — actual function placement and motivation
f=Fig(3,'Which functions run from faster memory?','Normal Release execution addresses; QSPI also holds the load copies of code that runs in RAM.')
for x,w,title,fill,col in [(60,812,'M4 · execution and communication','iol','io'),(908,832,'M7 · presentation and planning','uil','ui')]:
    f.rect(x,216,w,851,C[fill],C['line']);f.text(x+24,258,title,29,C[col],True)
f.rect(84,284,764,388,C['white'],C['line'])
f.text(108,323,'Internal SRAM · fast code region',28,C['io'],True)
f.text(108,359,'0x10000800 · 192 KiB reserved',20,C['muted'],mono=True)
items=[('STEP generation','StepGenerator, StepHardware, StepOutput'),('Motion evaluation','Executor, ProfileSchedule, Profile, Sequence'),('Motor / safety paths','StepperMotor, feedback, driver fault handling'),('Communication paths','Bridge, ServiceBridge, ActionBridge, transport'),('Runtime / helpers','ThreadX, clock, notification, selected libc/libgcc')]
for i,(a,b) in enumerate(items):
    yy=400+i*44;f.text(108,yy,a,20,C['io'],True);f.text(108,yy+22,b,18)
f.text(108,634,f'{fast:,} / 196,608 B · {100*fast/196608:.2f}% · {196608-fast:,} B remain',20,C['red'],True)
f.bar(108,648,716,fast/196608,'io',10)
f.rect(84,690,764,207,C['white'],C['line'])
f.text(108,729,'QSPI · ordinary M4 code',27,C['io'],True)
f.text(108,765,'0x90100000 · image partition',20,C['muted'],mono=True)
f.para(108,805,'Server dispatch, admission, worker construction and cold helpers run from QSPI. Timing-sensitive execution stays in internal SRAM; constructors do not consume the fast-code budget.',716,21)
f.rect(84,915,764,127,C['white'],C['line'])
f.text(108,952,'QSPI · remaining read-only material',26,C['purple'],True)
f.para(108,990,'Ordinary constants generally remain in flash; constants selected with fast code move to internal SRAM.',716,21)
# M7
f.rect(932,284,784,248,C['white'],C['line'])
f.text(956,323,'ITCM · selected TGX renderer routines',27,C['ui'],True)
f.text(956,359,'0x00000000 · 16,088 B linked',20,C['muted'],mono=True)
f.text(956,402,'_drawMesh     uber_shader     _discardBox',21,mono=True)
f.para(956,447,'Selected geometry / raster paths avoid external instruction fetches. The current allocation is below the 24 KiB renderer cap.',736,21)
f.rect(932,550,784,231,C['white'],C['line'])
f.text(956,589,'QSPI + M7 instruction cache',28,C['purple'],True)
f.text(956,625,'Ordinary application starts at 0x90400000',20,C['muted'],mono=True)
f.para(956,668,'GUI, CLI, Robot and Planner code execute here. The M7 caches instructions; the M4 does not have an equivalent instruction cache.',736,22)
f.rect(932,799,784,243,C['white'],C['line'])
f.text(956,838,'DTCM · planning stack and CPU data',27,C['ui'],True)
f.para(956,884,'The planning worker has a static 24 KiB DTCM stack. Its large planning arena is in SDRAM.',736,23)
f.line(956,951,1692,951,C['line'])
f.para(956,986,'A fast stack does not place the planner’s functions in ITCM.',736,23,C['ui'],True)
# exact distinction
f.rect(60,1091,1680,145,C['sharedl'],C['line'])
f.text(84,1132,'Important configuration distinction',27,C['shared'],True)
f.para(84,1175,'Normal Release: GIGA_M4_SDRAM_CODE=OFF. The motor-disconnected test fixture enables SDRAM cold code. Link audits trace fast roots and selected indirect calls; memory placement is not a timing guarantee.',1632,23)
f.save('03-function-placement','Sources: cmake/giga_fast_code.cmake; application_giga.cmake; linked ELF symbols / manifest; CMake cache for flashed test pair.', ['cmake/giga_fast_code.cmake','cmake/application_giga.cmake','CMakeLists.txt','build/giga-io-release/CMakeCache.txt','build/giga-release/images/manifest.json','build/motion-review/ram-audit.json'])

# 4 — evidence and remaining limits, never historical failures presented as current
f=Fig(4,'Motion baseline: evidence and remaining limits','Software correctness, electrical timing and mechanical qualification are separate questions.')
f.box(60,220,812,262,'Admission and ownership','One exclusive Motor / Axis / Robot family. Reservations include preparation and cleanup. Protocol, configuration, session, axis revisions and positions are checked before outputs are armed.','io','iol',ts=29,bs=24)
f.box(908,220,832,262,'Execution and stopping','M4 maintains immutable schedules with an installed brake. Explicit stop and lease loss select that branch locally. Abort / guard inhibition are separate fault paths and can invalidate referencing.','ui','uil',ts=29,bs=24)
f.box(60,510,812,291,'Memory remains a constraint','M4 fast code and D2 data are close to their linker limits. Each raw/homing worker reuses its reserved stack after joining. Command and executor stacks are budgeted separately; no steady-state motion buffers use the UI heap.','shared','sharedl',ts=29,bs=24)
f.box(908,510,832,291,'Open blocker: concurrent jog + axis move','The GIGA application can lose its ICC heartbeat during this overlap. UI reports the reason and shuts down; IO applies local loss handling. No HardFault was observed. The underlying scheduling / blocking mechanism is not yet isolated.','red','redl',ts=28,bs=23)
f.box(60,829,812,275,'Electrical captures pass','Raw: 18 / 18 / 9 pulses. Coordinated: 400 each. Controlled stop: 266 each. HIGH time: 5.00–5.04 µs. Coordinated skew: at most 42 ns. ENABLE remains inactive. These short tests peak near 711 pulses/s.','io','iol',ts=28,bs=23)
f.box(908,829,832,275,'Remaining verification gaps','Two native ASan/UBSan workflows still time out without a sanitizer memory report; their cause is unresolved. Lint findings remain. The 6,250-pulse/s ceiling, worst-case latency and powered mechanics are not qualified by these checks.','purple','purplel',ts=28,bs=23)
f.rect(60,1132,1680,104,C['white'],C['line'])
f.para(84,1170,'Use the review report for exact test outcomes and capture hashes. No diagram claims that a passing short capture proves a full-rate or mechanically safe operating envelope.',1632,24,C['ink'])
f.save('04-current-problems','Sources: Controller / Ownership / StepperMotor; OperatingEnvelope; review report and hardware captures.', ['src/io/motion/Controller.cpp','src/io/motion/ControllerAdmission.cpp','src/io/motion/StepperMotorCommands.cpp','src/common/motion/OperatingEnvelope.hpp','docs/motion-current-state/review.json'])
print('Created four SVG figures in',OUT)
