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
     t=k.PCB_VIA(b);t.SetPosition(xy(x,y));t.SetWidth(k.FromMM(diameter));t.SetDrill(k.FromMM(drill));t.SetViaType(k.VIATYPE_THROUGH);t.SetLayerPair(k.F_Cu,k.B_Cu);t.SetNet(b.FindNet(net));t.SetLocked(True);t.SetFrontTentingMode(k.TENTING_MODE_NOT_TENTED);t.SetBackTentingMode(k.TENTING_MODE_NOT_TENTED);b.Add(t)
    # Full-current module output and backlight bus. VIN routing is completed
    # after connector escapes; never use a signal-width trace for this output.
    track('+3V3_PANEL',[(10,20.92),(12,20.92),(12,31.5),(12.975,32.475),(34,32.475)],1.2)
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
