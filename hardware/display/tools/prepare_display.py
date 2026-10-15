"""Two-layer display-board power paths and connector escapes before routing.

The final checked-in PCB remains authoritative. A 0.55/0.25 mm via lets a
0.15 mm track pass the staggered 1 mm via rows at 0.15 mm clearance.
"""
import pcbnew as k


def prepare_display(b):
    fps={f.GetReference():f for f in b.GetFootprints()}
    def xy(x,y):return k.VECTOR2I(k.FromMM(x),k.FromMM(y))
    def track(net,points,w=.18,l=k.F_Cu):
     for a,c in zip(points,points[1:]):
      t=k.PCB_TRACK(b);t.SetStart(xy(*a));t.SetEnd(xy(*c));t.SetWidth(k.FromMM(w));t.SetLayer(l);t.SetNet(b.FindNet(net));t.SetLocked(True);b.Add(t)
    def via(net,x,y,diameter=.65,drill=.3):
     t=k.PCB_VIA(b);t.SetPosition(xy(x,y));t.SetWidth(k.FromMM(diameter));t.SetDrill(k.FromMM(drill));t.SetViaType(k.VIATYPE_THROUGH);t.SetLayerPair(k.F_Cu,k.B_Cu);t.SetNet(b.FindNet(net));t.SetLocked(True);t.SetFrontTentingMode(k.TENTING_MODE_TENTED);t.SetBackTentingMode(k.TENTING_MODE_TENTED);b.Add(t)
    # Broad current paths: terminal, protection, bulk capacitors, panel backlight.
    track('+3V3_INPUT',[(6,24),(6,26),(8,28),(10.7125,28),(12.7125,26)],1.2)
    track('+3V3_FUSED',[(14.2875,26),(16.9375,26),(16.9375,29)],.8)
    track('+3V3_FUSED',[(16.9375,29),(17.5375,28.4),(18.3,28.4)],.8)
    track('+3V3_FUSED',[(16.9375,33),(17.8,33),(18.3,32.5)],.8)
    for y in [28.4,32.5]:via('+3V3_FUSED',18.3,y)
    track('+3V3_FUSED',[(18.3,28.4),(18.3,32.5)],.8,k.B_Cu)
    for y in [29.95,33.95]:
     track('+3V3_PANEL',[(15.0625,y),(13.8,y)],.6);via('+3V3_PANEL',13.8,y)
    track('+3V3_PANEL',[(13.8,29.95),(13.8,33.95)],.8,k.B_Cu)
    track('+3V3_PANEL',[(15.0625,29.95),(18.5,29.95),(21.025,32.475),(22,32.475)],.8)
    track('+3V3_PANEL',[(22,32.475),(34,32.475)],1.5)
    track('+3V3_PANEL',[(18.5,29.95),(19.3,29.15),(19.3,23),(22.5,23)],.8)
    for x in [22.5,23.3]:via('+3V3_PANEL',x,23)
    track('+3V3_PANEL',[(22.5,23),(23.3,23)],.5,k.B_Cu)
    track('+3V3_PANEL',[(22.5,23),(22.5,21.85),(23.5,21.85)],.5,k.B_Cu)
    track('+3V3_PANEL',[(22.5,23),(22.5,24.15),(23,24.15)],.5,k.B_Cu)
    for x in [22.5,23,23.5]:track('+3V3_PANEL',[(x,21.85),(x,21.05)],.25,k.B_Cu)
    for x in [22.5,23]:track('+3V3_PANEL',[(x,24.15),(x,24.95)],.25,k.B_Cu)
    # Terminal and bulk ground: wide copper with paired terminal/connector vias.
    track('GND',[(9.5,24),(9.5,22.5),(10.3,22.5)],1.2)
    for x in [9.5,10.3]:via('GND',x,22.5)
    for x in [22,26,30,34]:track('GND',[(x,29.525),(x,28.5)],.8);via('GND',x,28.5)
    for y in [21.05,24.95]:
     track('GND',[(20.2,y),(21.5,y),(22,y)],.25,k.B_Cu);via('GND',20.2,y)
    # FFC ground rail; signal via pitch is 1mm, with no via fence underneath it.
    for pad in fps['J2'].Pads():
     x,y=k.ToMM(pad.GetPosition().x),k.ToMM(pad.GetPosition().y);net=pad.GetNetname()
     if pad.GetNumber().startswith('MP'):
      track('GND',[(x,y),(x,y+1.5)],.4);via('GND',x,y+1.5)
     elif net=='GND':track(net,[(x,y),(x,8.3)])
     else:track(net,[(x,y),(x,5.3)]);via(net,x,5.3)
    track('GND',[(7.75,8.3),(47.25,8.3)],.4)
    for x in [7.75,27.25,47.25]:via('GND',x,8.3)
    # Both Hirose rows get explicit staggered 0.55/0.25 mm escapes before routing.
    for pad in fps['J1'].Pads():
     n=int(pad.GetNumber());x,y=k.ToMM(pad.GetPosition().x),k.ToMM(pad.GetPosition().y);net=pad.GetNetname()
     if n<=8 or n==10:continue
     if n%2==0:yy=19.5 if n%4==0 else 18.6
     else:yy=26.5 if n%4==1 else 27.4
     track(net,[(x,y),(x,yy)],.15,k.B_Cu);via(net,x,yy,.55,.25)
