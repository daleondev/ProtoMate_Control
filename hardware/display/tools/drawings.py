#!/usr/bin/env python3
"""Fabrication, cable and assembly instruction drawings (vector PDF)."""
import csv,json,textwrap
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle,Polygon,Circle
from matplotlib.backends.backend_pdf import PdfPages
from design import ROOT,REVISION,BOARDS,release_name,copper_layers,SIGNALS,ribbon_nets,GEOMETRY,board_outline

def sheet(title, revision=REVISION, date="2026-10-15"):
    fig=plt.figure(figsize=(11.69,8.27));fig.text(.04,.95,title,fontsize=16,weight='bold')
    fig.text(.04,.025,f'ProtoMate | Revision {revision} | {date} | Dimensions in mm | Physical operation not yet verified',fontsize=8)
    return fig
def notes(fig,lines,x=.06,y=.86,width=115,size=10):
    for line in lines:
        for t in textwrap.wrap(line,width) or ['']:
            fig.text(x,y,t,fontsize=size);y-=.026
        y-=.012

def cable():
    with (ROOT/'cable-conductors.csv').open('w') as f:
        w=csv.writer(f,lineterminator="\n");w.writerow(['GIGA_J1','Display_J2','Net','Display_J1_contacts'])
        nets=ribbon_nets()
        for p in range(1,81):
            net=nets[str(p)]
            display=next((str(s['panel']) for s in SIGNALS if 'LCD_'+s['name']==net),'')
            if net=='+3V3_PANEL':display='5,6,7,8,10,49'
            if net=='GND':display='1,2,3,4,11,36,40,42,50'
            w.writerow([p,81-p,net,display])
    with PdfPages(ROOT/'cable-specification.pdf') as pdf:
        fig=sheet('FFC-80-A-100 — cable procurement and orientation')
        ax=fig.add_axes([.08,.47,.84,.39]);ax.set_xlim(-10,110);ax.set_ylim(-5,49);ax.set_aspect('equal');ax.axis('off')
        ax.add_patch(Rectangle((0,0),100,40.5,facecolor='#e5e9ed',edgecolor='black'))
        for i in range(80):
            y=.5+i*.5;ax.plot([3.5,96.5],[y,y],color='#b7bec5',lw=.3)
            for x in [0,96.5]:ax.add_patch(Rectangle((x,y-.15),3.5,.3,color='#ba9239'))
        for x in [0,95.5]:ax.add_patch(Rectangle((x,0),4.5,40.5,fill=False,edgecolor='#2876b8',ls='--'))
        ax.annotate('',(0,44),(100,44),arrowprops={'arrowstyle':'<->'});ax.text(50,45,'100 maximum overall length',ha='center')
        ax.text(-2,20.25,'40.50 ±0.07',rotation=90,ha='right',va='center')
        ax.text(0,-3,'GIGA end: 1 at this lower edge',ha='left',fontsize=9)
        ax.text(100,-3,'Display end: 80 at the same edge',ha='right',fontsize=9)
        notes(fig,[
            '80 parallel conductors; pitch 0.50 mm; same-side exposed contacts (Type A). Gold-plated mating finish. Straight-sided ends; no latch ears or custom side notches.',
            'Mating thickness 0.30 ±0.05 mm. Stiffener thickness at least 0.188 mm. Exposed contact length at least 3.5 mm; stiffener length at least 4.5 mm. All dimensions must also meet the Hirose FH28 cable-end drawing.',
            'Dashed blue regions show the stiffeners on the opposite face. Gold regions are both on the face shown. Fit that exposed-contact face to the FH28 contact face shown in its manufacturer drawing; do not identify orientation from blue stiffener colour alone.',
            'Both PCB sockets: Hirose FH28H-80S-0.5SH(05). With the two top-mounted sockets facing each other, GIGA pin n reaches display pin 81-n. Confirm this by continuity before applying power.',
            '24V power: GIGA 1/3 <-> display 80/78. Panel 3.3V reference return: GIGA 5/7/9 <-> display 76/74/72. 40 ground conductors. PCLK 31 <-> 50 keeps ground on both sides.',
            'Rated at least 60V and 0.5A per conductor; copper cross-section >=0.0105 mm2 (e.g. 0.30 x 0.035 mm). Procure as custom drawing FFC-80-A-100 revision C if no catalogue cable meets every requirement. The cable is not part of either PCB SMT BOM.'
        ],y=.43,width=132,size=9)
        pdf.savefig(fig);plt.close(fig)

def fabrication(name):
    out=ROOT/'manufacturing'/release_name(name);out.mkdir(parents=True,exist_ok=True)
    with PdfPages(out/'fabrication-and-assembly-notes.pdf') as pdf:
        fig=sheet(name+' — fabrication drawing', BOARDS[name]['revision'], BOARDS[name]['date'])
        ax=fig.add_axes([.05,.38,.62,.5]);ax.set_aspect('equal');ax.axis('off')
        if name=='display-adapter':
            contour=[(0,0),(55,0),(55,35),(0,35)];holes=[(3,12),(52,12),(3,32),(52,32)];diam=2.2;width,height=55,35;thick=1.0
        else:
            contour=board_outline(name)
            holes=[p for p in GEOMETRY['mounting_holes'] if p[0]>88 or p[0]<17 and (p[1]<5 or p[1]>49)];diam=3.2;width,height=111,53.34;thick=1.6
        ax.add_patch(Polygon(contour,fill=False,lw=1.5));ax.set_xlim(min(x for x,y in contour)-8,max(x for x,y in contour)+8);ax.set_ylim(height+9,-9)
        for x,y in holes:ax.add_patch(Circle((x,y),diam/2,fill=False));ax.text(x+2,y+2,f'({x:.2f}, {y:.2f})',fontsize=6)
        left=min(x for x,y in contour);right=max(x for x,y in contour)
        ax.annotate('',(left,-5),(right,-5),arrowprops={'arrowstyle':'<->'});ax.text((left+right)/2,-6,f'{width:.2f}',ha='center')
        ax.annotate('',(right+5,0),(right+5,height),arrowprops={'arrowstyle':'<->'});ax.text(right+6,height/2,f'{height:.2f}',rotation=90,va='center')
        fig.text(.70,.90,f'Finished thickness: {thick:.1f} mm\n{BOARDS[name]["layers"]} layers, FR-4\n1 oz copper, all layers\nENIG\nGreen mask / white silk\nIPC Class 2\nElectrical test: 100%',fontsize=11,linespacing=1.8,va='top')
        notes(fig,[
            f'Layer order: {" / ".join(copper_layers(name))}. Routed Edge.Cuts and drill files are authoritative. Nominal stackup is embedded in the KiCad board; equivalent FR-4 laminate construction is acceptable at the specified finished thickness.',
            f'Minimum track and copper clearance 0.15 mm; through via drill {BOARDS[name]["via_drill"]:.2f} mm, diameter {BOARDS[name]["via_diameter"]:.2f} mm, annular ring 0.15 mm. Tented vias both sides, no via-in-pad. Preserve fine-pitch solder-mask webs (minimum 0.13 mm).',
            'PCBWay to add assembly rails/panelization without changing individual outlines or mounting coordinates. Do not mirror bottom fabrication files. PTH and NPTH files are separate.',
            'Origin in Gerbers, drills and placement: X=0, Y=lower board edge; positive Y upward. Drawing coordinates above use KiCad top-left Y-down for mechanical reference.',
            'Stencil: stepped 0.15 mm at FH28 and 0.10–0.12 mm at 0402/TSSOP parts. Hirose paste apertures are already reduced in the supplied paste layers. No substitutions for connector suffixes or header tail lengths.'
        ],y=.33,width=140,size=9)
        pdf.savefig(fig);plt.close(fig)
        fig=sheet(name+' — assembly instructions', BOARDS[name]['revision'], BOARDS[name]['date'])
        lines=[
            'Use assembly-F.pdf and assembly-B.pdf for reference designators, pin numbers and component orientation. Bottom drawing is mirrored to show the actual bottom view. Native KiCad placement rotations are retained in placement.csv.',
            'All components in bom-pcbway-smt.csv are FIT. No DNP SMT components. Parts marked HAND in bom-all.csv are supplied/installed separately and must not appear in the SMT placement file.',
            'Inspect every 0.5 mm connector contact for bridges, insufficient solder and coplanarity. Check soldermask webs and that no solder or un-tented via protrudes into the Hirose underside/mating keepout.',
        ]
        if name=='display-adapter':lines += [
            'J1 DF23C-50DP-0.5V(92) is BOTTOM mounted. Viewed from the PCB top: pin 1 is lower left of the contact array, pin 2 above it, pin 49 lower right. Cross-check Smartwin rear-view Detail A before mating.',
            'J2 FH28H is TOP mounted, mouth facing the upper board edge, pin 1 at the right end viewed from the top. Fit the cable with the specified same-side contact orientation.',
            'U1 TRACO TSR 2-2433 is HAND soldered on TOP. Pin 1 = 24V in, pin 2 = GND, pin 3 = 3.3V out. Follow the pin-1 marking; the 12V converter is NOT interchangeable.',
            'After soldering U1, clip its leads to at most 0.8 mm below the PCB, preserving the solder joints. Untrimmed module leads protrude below the nominal display mating plane. The STEP assembly model shows trimmed leads.',
            'Support the PCB on four M2 insulating mounts and clamp the FFC/the ribbon to the enclosure. The display flex and DF23 must carry no cable or PCB weight. Maintain nominal 1.5 mm mating-board spacing at DF23.'
        ]
        else:lines += [
            'J1 FH28H is TOP mounted, cable mouth facing the right board edge. Pin 1 is at the lower end when the board is viewed from the top. Check the silkscreen 1/80 markings.',
            'J10 Phoenix 1751248: HAND solder, wire entry faces left into the central opening. Pin 1 = 24V INPUT, pin 2 = GND. U8 TSR 1-24120 is also HAND soldered; pin 1 = protected 24V, pin 2 = GND, pin 3 = 12V.',
            'U7 thermal pad and RTN belong to their own copper island; never solder-bridge them to GND. D1 is bidirectional; D2/D3 cathodes follow their band and pad 1.',
            'J2–J9 stacking headers are HAND soldered, sockets above the PCB and tails below. J2–J8: Samtec SSQ -03, 10 mm tails. J9: SSQ-112-04-G-D, 14.83 mm tails. Verify exact suffixes in the full BOM.',
            'GIGA top to carrier underside: 13.0 mm. Lower two mounting positions use M2 insulating support hardware; upper holes are alignment clearances without screw heads. Support the cable end independently.',
            'Do not bridge isolated pass-through contacts to adapter ground or power. Motor/EtherCAT contacts remain accessible through the stacking sockets and are electrically unconnected on this PCB.'
        ]
        lines += ['First-board power, timing, touch and supply-sequencing tests are listed in README.md. They have not been performed.']
        notes(fig,lines,width=130,size=10)
        pdf.savefig(fig);plt.close(fig)

if __name__=='__main__':
    cable()
    for n in ['display-adapter','giga-adapter']:fabrication(n)
