#!/usr/bin/env python3
"""Fabrication, cable and assembly instruction drawings (vector PDF)."""
import csv,json,textwrap,math
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle,Polygon,Circle
from matplotlib.backends.backend_pdf import PdfPages
from design import ROOT,REVISION,BOARDS,release_name,copper_layers,SIGNALS,ribbon_nets,GEOMETRY,board_outline
from aisler import BRIDGES,k

def sheet(title, revision=REVISION, date="2026-10-22"):
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
        for p in range(1,61):
            net=nets[str(p)]
            display=next((str(s['panel']) for s in SIGNALS if 'LCD_'+s['name']==net),'')
            if net=='+3V3_PANEL':display='10,49; logic supply only'
            if net=='PANEL_READY':display='Adapter supervisors -> PI1 / D70; no direct panel pin'
            if net=='GND':display='1,2,3,4,11,36,40,42,50'
            w.writerow([p,61-p,net,display])
    with PdfPages(ROOT/'cable-specification.pdf') as pdf:
        fig=sheet('Molex 15018-0563 — 60-way cable and orientation')
        ax=fig.add_axes([.08,.48,.84,.38]);ax.set_xlim(-10,112);ax.set_ylim(-5,40);ax.set_aspect('equal');ax.axis('off')
        # Orientation diagram, not a replacement for the manufacturer's cutting drawing.
        ax.add_patch(Rectangle((0,0),102,30.5,facecolor='#e5e9ed',edgecolor='black'))
        for x in [4,96]:
            ax.add_patch(Rectangle((x,-1),2,32.5,facecolor='#e5e9ed',edgecolor='black'))
        for i in range(60):
            y=.5+i*.5;ax.plot([3.5,98.5],[y,y],color='#b7bec5',lw=.3)
            for x in [0,98.5]:ax.add_patch(Rectangle((x,y-.15),3.5,.3,color='#ba9239'))
        for x in [0,97.5]:ax.add_patch(Rectangle((x,0),4.5,30.5,fill=False,edgecolor='#2876b8',ls='--'))
        ax.annotate('',(0,35),(102,35),arrowprops={'arrowstyle':'<->'});ax.text(51,36,'102 nominal; 106 maximum design budget',ha='center')
        ax.text(-2,15.25,'30.5 end width',rotation=90,ha='right',va='center')
        ax.text(0,-3,'GIGA end: project pin 1 at this edge',ha='left',fontsize=8)
        ax.text(102,-3,'Display end: project pin 60 at same edge',ha='right',fontsize=8)
        notes(fig,[
            'Order Molex 15018-0563 (Mouser 538-15018-0563), quantity 1: 60 parallel conductors, 0.50 mm pitch, same-side gold contacts (Type A), 102 mm nominal length. Ear tabs are required by the matched connector.',
            'Both sockets: Molex 505110-6091 (Mouser 538-505110-6091), quantity 2. Bottom contacts, flip latch. Cable mating thickness 0.30 +/-0.05 mm. Manufacturer end/tab geometry is authoritative; this view only explains orientation.',
            'Gold contacts are on the face shown; dashed blue regions indicate opposite-face stiffeners. In both top-mounted sockets the exposed contacts face the PCB. Do not identify contact orientation from stiffener colour alone.',
            'With facing sockets, project GIGA pin n reaches display pin 61-n. Project numbering follows the PCB drawings. Before power, check end-to-end continuity and latch both ends without twisting or stressing the cable.',
            '24V: GIGA 1/2 <-> display 60/59. Panel 3.3V reference: GIGA 5/6/7 <-> display 56/55/54. READY: 9 <-> 52. 19 ground conductors; PCLK 23 <-> 38 is guarded by grounds 22/24 <-> 39/37.',
            'Cable: 60V, 0.5A/contact; connector: 50VAC/DC, 0.5A/contact. Copper 0.30 x 0.10 mm. Power budget uses 106 mm and two return conductors conservatively. No live mating. Cable is included separately in the procurement order list.'
        ],y=.43,width=132,size=9)
        pdf.savefig(fig);plt.close(fig)

def fabrication(name):
    out=ROOT/'manufacturing'/release_name(name);out.mkdir(parents=True,exist_ok=True)
    with PdfPages(out/'fabrication-and-assembly-notes.pdf') as pdf:
        fig=sheet(name+' — fabrication drawing', BOARDS[name]['revision'], BOARDS[name]['date'])
        ax=fig.add_axes([.05,.38,.62,.5]);ax.set_aspect('equal');ax.axis('off')
        if name=='display-adapter':
            contour=[(0,0),(55,0),(55,50),(0,50)];holes=[(3,12),(52,12),(3,47),(52,47)];diam=2.2;width,height=55,50;thick=1.6
        else:
            contour=board_outline(name)
            holes=[p for p in GEOMETRY['mounting_holes'] if p[0]>88 or p[0]<17 and (p[1]<5 or p[1]>49)];diam=3.2;width,height=111,53.34;thick=1.6
        board=k.LoadBoard(str(ROOT/name/(name+'.kicad_pcb')))
        for edge in board.GetDrawings():
            if edge.GetLayer()!=k.Edge_Cuts:continue
            def xy(p):return k.ToMM(p.x),k.ToMM(p.y)
            a,c=xy(edge.GetStart()),xy(edge.GetEnd())
            points=[a,c]
            if edge.GetShape()==k.SHAPE_T_ARC:
                x,y=xy(edge.GetCenter());r=math.hypot(a[0]-x,a[1]-y)
                start=math.atan2(a[1]-y,a[0]-x);end=math.atan2(c[1]-y,c[0]-x)
                delta=(end-start+math.pi)%(2*math.pi)-math.pi
                points=[(x+r*math.cos(start+delta*i/24),y+r*math.sin(start+delta*i/24))for i in range(25)]
            ax.plot([p[0]for p in points],[p[1]for p in points],color='black',lw=1.5)
        for a,c in BRIDGES[name]:ax.plot([a[0],c[0]],[a[1],c[1]],color='red',lw=1.5)
        ax.set_xlim(min(x for x,y in contour)-8,max(x for x,y in contour)+8);ax.set_ylim(height+9,-9)
        for x,y in holes:
            ax.add_patch(Circle((x,y),diam/2,fill=False))
            right_side=x>(min(px for px,py in contour)+max(px for px,py in contour))/2
            ax.text(x-2 if right_side else x+2,y+2,f'({x:.2f}, {y:.2f})',fontsize=6,ha='right' if right_side else 'left')
        left=min(x for x,y in contour);right=max(x for x,y in contour)
        ax.annotate('',(left,-5),(right,-5),arrowprops={'arrowstyle':'<->'});ax.text((left+right)/2,-6,f'{width:.2f}',ha='center')
        ax.annotate('',(right+5,0),(right+5,height),arrowprops={'arrowstyle':'<->'});ax.text(right+6,height/2,f'{height:.2f}',rotation=90,va='center')
        fig.text(.70,.90,f'Finished thickness: {thick:.1f} mm +/-10%\n{BOARDS[name]["layers"]} layers, FR-4\n35 um copper, all layers\nENIG\nGreen mask / white silk\nIPC Class 2\nElectrical test: 100%\nRed: bridge suggestions',fontsize=11,linespacing=1.8,va='top')
        notes(fig,[
            f'Layer order: {" / ".join(copper_layers(name))}. Routed Edge.Cuts and drill files are authoritative. '+('Four-layer dielectrics: 0.14 / 1.20 / 0.14 mm, nominal finished 1.6 mm. GIGA concave milling radii: 0.9 mm.' if BOARDS[name]['layers']==4 else 'AISLER controls laminate construction at the specified finished thickness.'),
            f'Minimum track 0.15 mm; copper clearance {0.125 if name=="display-adapter" else 0.15:.3f} mm. Through via drill {BOARDS[name]["via_drill"]:.2f} mm, diameter {BOARDS[name]["via_diameter"]:.2f} mm, annular ring >=0.15 mm. Exposed vias, no via-in-pad. Zero source mask expansion; processed fine-pitch webs >=0.10 mm. Component-hole rings >=0.30 mm.',
            'AISLER bare boards only; ENIG on both boards. Use the ODB++ upload ZIP, including the aisler-bridges document layer. Keep the closed profile unchanged. PTH and NPTH drills are separate. No assembly or stencil is ordered.',
            'Origin in Gerbers, drills and placement: X=0, Y=lower board edge; positive Y upward. Drawing coordinates above use KiCad top-left Y-down for mechanical reference.',
            'Optional paste reference only (no stencil ordered): 0.10 mm at Molex 505110, with 100% land-area openings; 0.10–0.12 mm at 0402/TSSOP parts. DF23 paste follows its separate manufacturer drawing. No substitutions for connector suffixes or header tail lengths.'
        ],y=.33,width=140,size=9)
        pdf.savefig(fig);plt.close(fig)
        fig=sheet(name+' — assembly instructions', BOARDS[name]['revision'], BOARDS[name]['date'])
        lines=[
            'Use assembly-F.pdf and assembly-B.pdf for reference designators, pin numbers and component orientation. Bottom drawing is mirrored to show the actual bottom view. Native KiCad placement rotations are retained in placement.csv.',
            'All components in bom-all.csv are FIT and installed separately by the builder. bom-smd.csv and placement.csv are placement aids only. HAND denotes through-hole installation; no assembly service is included.',
            'Inspect every 0.5 mm connector contact for bridges, insufficient solder and coplanarity. Check soldermask webs and that no solder or exposed via intrudes into the DF23 contact-band keepout.',
        ]
        if name=='display-adapter':lines += [
            'J1 DF23C-50DP-0.5V(92) is BOTTOM mounted. Viewed from the PCB top: pin 1 is lower left of the contact array, pin 2 above it, pin 49 lower right. Cross-check Smartwin rear-view Detail A before mating.',
            'J2 Molex 505110 is TOP mounted, mouth facing the upper board edge, pin 1 at the right end viewed from the top. Fit the cable with the specified same-side contact orientation.',
            'U1 TRACO TSR 2-2433 is HAND soldered on TOP. Pin 1 = 24V in, pin 2 = GND, pin 3 = 3.3V out. Follow the pin-1 marking; the 12V converter is NOT interchangeable.',
            'After soldering U1 and U2, clip their leads to at most 0.8 mm below the PCB, preserving the solder joints. Untrimmed module leads protrude below the nominal display mating plane. The STEP assembly model shows trimmed leads.',
            'U2 is TSR 1-2450 (5V); U3 is TPS7A91 (3.296V). Solder the WSON exposed pad with paste and controlled hot air/reflow. No via filling is required. Never swap converter types.',
            'Support the PCB on four M2 insulating mounts and clamp the FFC/the ribbon to the enclosure. The display flex and DF23 must carry no cable or PCB weight. Maintain nominal 1.5 mm mating-board spacing at DF23, independent of the 1.6 mm PCB thickness.'
        ]
        else:lines += [
            'J1 Molex 505110 is TOP mounted, cable mouth facing the right board edge. Pin 1 is at the lower end when the board is viewed from the top. Check the silkscreen 1 marking and the numbered assembly drawing.',
            'J10 DC-005: HAND solder, 5.5/2.1mm barrel opening faces left into the service cutout. CENTRE POSITIVE 24V: pin 1 = +24V, pins 2/3 = GND. Three plated slots: width 1.2mm, lengths 4.0/3.6/4.0mm. U8 TSR 1-24120 is also HAND soldered.',
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
