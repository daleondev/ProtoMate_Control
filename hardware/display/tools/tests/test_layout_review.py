"""Regression fixtures for known layout-review blind spots; run with pcbnew."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import pcbnew as k
from audit_workmanship import scan, angle_error, simple_paths
from layout_policy import capture, check_protected, allow_silkscreen_removal, record_placement_change, record_thermal_angles
from review_atlas import validate
from finish_board import finish
from prepare_routes import prepare

k.SwigPyIterator.next=k.SwigPyIterator.__next__


class LayoutReviewTests(unittest.TestCase):
    def setUp(self):
        self.folder=tempfile.TemporaryDirectory()
        self.path=Path(self.folder.name)/'fixture.kicad_pcb'
        self.board=k.BOARD();self.board.SetCopperLayerCount(4)
        self.nets={}

    def tearDown(self):
        self.folder.cleanup()

    def net(self,name):
        if name not in self.nets:
            net=k.NETINFO_ITEM(self.board,name);self.board.Add(net);self.nets[name]=net
        return self.nets[name]

    def track(self,a,b,net='SIGNAL',layer=k.F_Cu,width=.15):
        t=k.PCB_TRACK(self.board);t.SetStart(k.VECTOR2I(k.FromMM(a[0]),k.FromMM(a[1])))
        t.SetEnd(k.VECTOR2I(k.FromMM(b[0]),k.FromMM(b[1])))
        t.SetWidth(k.FromMM(width));t.SetLayer(layer);t.SetNet(self.net(net));self.board.Add(t)
        return t

    def save(self):
        k.SaveBoard(str(self.path),self.board)

    def findings(self):
        self.save();return scan(self.path)['findings']

    def test_all_nets_receive_angle_checks(self):
        for i,net in enumerate(('SIGNAL','GND','PWR_RTN','+24V_DISPLAY')):
            self.track((0,i*5),(2,i*5+.4),net)
        findings=self.findings()
        self.assertEqual({f['net'] for f in findings if f['kind']=='off_angle'},set(self.nets))

    def test_inner_signal_is_rejected(self):
        self.track((0,0),(5,0),layer=k.In2_Cu)
        self.assertIn('inner_track',{f['kind'] for f in self.findings()})

    def test_duplicate_and_partial_overlap(self):
        self.track((0,0),(2,0));self.track((2,0),(0,0));self.track((1,0),(4,0))
        kinds={f['kind'] for f in self.findings()}
        self.assertIn('duplicate_track',kinds);self.assertIn('collinear_overlap',kinds)

    def test_short_pad_escape_is_review_not_automatic_failure(self):
        self.track((0,0),(.2,0))
        f=next(f for f in self.findings() if f['kind']=='short_segment')
        self.assertEqual(f['severity'],'review')

    def test_legitimate_angular_route(self):
        self.track((0,0),(3,0));self.track((3,0),(5,2))
        self.assertFalse(self.findings())

    def test_hairpin_is_identified(self):
        self.track((0,0),(5,0));self.track((5,0),(3,2))
        self.assertIn('hairpin',{f['kind'] for f in self.findings()})

    def test_edge_only_pad_connection_is_rejected_on_ground(self):
        fp=k.FOOTPRINT(self.board);fp.SetReference('J1');self.board.Add(fp)
        p=k.PAD(fp);p.SetNumber('1');p.SetAttribute(k.PAD_ATTRIB_SMD)
        p.SetShape(k.PAD_SHAPE_RECT);p.SetSize(k.VECTOR2I(k.FromMM(1),k.FromMM(1)))
        layers=k.LSET();layers.AddLayer(k.F_Cu)
        p.SetPosition(k.VECTOR2I(0,0));p.SetLayerSet(layers);p.SetNet(self.net('GND'));fp.Add(p)
        self.track((-.54,-2),(-.54,2),'GND')
        self.assertIn('pad_edge_touch',{f['kind'] for f in self.findings()})

    def test_protection_allows_routing_but_not_graphics_changes(self):
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        self.track((1,1),(5,1));self.save();check_protected(self.path,baseline)
        text=k.PCB_TEXT(self.board);text.SetText('Changed silk');text.SetLayer(k.F_SilkS);self.board.Add(text);self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)
        with self.assertRaises(RuntimeError):capture(self.path,baseline)

    def test_stale_visual_review_is_rejected(self):
        self.save();p=self.path.with_suffix('.json')
        p.write_text(json.dumps({'source':{str(self.path):'wrong'},'board':str(self.path)}))
        with self.assertRaisesRegex(RuntimeError,'changed'):validate(p)

    def test_authorized_silk_removal_preserves_original_protection(self):
        text=k.PCB_TEXT(self.board);text.SetText('CLK');text.SetLayer(k.F_SilkS);self.board.Add(text)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        original=json.loads(baseline.read_text())['protected_sha256']
        allow_silkscreen_removal(self.path,baseline,text.m_Uuid.AsString(),
                                'Clock label overlaps a necessary signal-via mask opening')
        self.board.Remove(text);self.save();check_protected(self.path,baseline)
        self.assertEqual(json.loads(baseline.read_text())['protected_sha256'],original)
        other=k.PCB_TEXT(self.board);other.SetText('Changed');other.SetLayer(k.F_SilkS);self.board.Add(other)
        self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)

    def test_silk_permission_cannot_remove_mechanics(self):
        text=k.PCB_TEXT(self.board);text.SetText('Mechanical');text.SetLayer(k.Dwgs_User);self.board.Add(text)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        with self.assertRaisesRegex(ValueError,'Only silkscreen'):
            allow_silkscreen_removal(self.path,baseline,text.m_Uuid.AsString(),
                                    'This is not a valid silkscreen exception')

    def test_footprint_silk_exceptions_keep_pad_and_placement_protected(self):
        fp=k.FOOTPRINT(self.board);fp.SetReference('U1');self.board.Add(fp)
        graphics=[]
        for x in (1,2):
            g=k.PCB_SHAPE(fp);g.SetShape(k.SHAPE_T_SEGMENT);g.SetLayer(k.F_SilkS)
            g.SetStart(k.VECTOR2I(k.FromMM(x),0));g.SetEnd(k.VECTOR2I(k.FromMM(x),k.FromMM(1)))
            fp.Add(g);graphics.append(g)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        for g in graphics:
            allow_silkscreen_removal(self.path,baseline,g.m_Uuid.AsString(),
                                    'Package outline crosses an exposed transition via')
            fp.Remove(g);self.save();check_protected(self.path,baseline)
        fp.SetPosition(k.VECTOR2I(k.FromMM(1),0));self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)

    def test_candidate_paths_are_octilinear(self):
        for path in simple_paths((0,0),(7,3)):
            for a,b in zip(path,path[1:]):self.assertLess(angle_error(a,b),.001)

    def test_authorized_placement_preserves_pad_geometry(self):
        fp=k.FOOTPRINT(self.board);fp.SetReference('R1');self.board.Add(fp)
        pad=k.PAD(fp);pad.SetNumber('1');fp.Add(pad)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        source=self.path.parent/'original.kicad_pcb';source.write_bytes(self.path.read_bytes())
        fp.SetOrientationDegrees(180);fp.SetPosition(k.VECTOR2I(k.FromMM(5),k.FromMM(7)));self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)
        record_placement_change(source,self.path,baseline,{'R1':(5,7,180)},
                                'Move the pull-down to make room for an ordered signal fanout')
        check_protected(self.path,baseline);check_protected(source,baseline)
        pad.SetSize(k.VECTOR2I(k.FromMM(2),k.FromMM(2)));self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)

    def test_placement_permission_rejects_extra_edits(self):
        fp=k.FOOTPRINT(self.board);fp.SetReference('R1');self.board.Add(fp)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        source=self.path.parent/'original.kicad_pcb';source.write_bytes(self.path.read_bytes())
        fp.SetPosition(k.VECTOR2I(k.FromMM(5),k.FromMM(7)))
        text=k.PCB_TEXT(self.board);text.SetText('Unrequested change');self.board.Add(text);self.save()
        with self.assertRaisesRegex(RuntimeError,'beyond'):
            record_placement_change(source,self.path,baseline,{'R1':(5,7,0)},
                                    'Move the pull-down to make room for an ordered signal fanout')

    def test_thermal_orientation_does_not_authorize_gap_or_pad_changes(self):
        fp=k.FOOTPRINT(self.board);fp.SetReference('J1');self.board.Add(fp)
        pad=k.PAD(fp);pad.SetNumber('1');pad.SetNet(self.net('GND'));fp.Add(pad)
        self.save();baseline=self.path.with_suffix('.json');capture(self.path,baseline)
        source=self.path.parent/'original.kicad_pcb';source.write_bytes(self.path.read_bytes())
        pad.SetThermalSpokeAngleDegrees(45);self.save()
        record_thermal_angles(source,self.path,baseline,{('J1','1'):45},
                              'Orient thermal spokes into the available ground copper corridors')
        check_protected(self.path,baseline);check_protected(source,baseline)
        pad.SetThermalGap(k.FromMM(.1));self.save()
        with self.assertRaises(RuntimeError):check_protected(self.path,baseline)

    def test_short_staircase_is_a_review_candidate(self):
        self.track((0,0),(2,0));self.track((2,0),(2.2,.2));self.track((2.2,.2),(4,.2))
        finding=next(f for f in self.findings() if f['kind']=='short_zigzag')
        self.assertEqual(finding['severity'],'review')

    def test_unconnected_signal_via_is_flagged(self):
        via=k.PCB_VIA(self.board);via.SetNet(self.net('SIGNAL'))
        via.SetPosition(k.VECTOR2I(k.FromMM(2),k.FromMM(2)))
        via.SetWidth(k.FromMM(.65));via.SetDrill(k.FromMM(.3))
        via.SetLayerPair(k.F_Cu,k.B_Cu);via.SetViaType(k.VIATYPE_THROUGH);self.board.Add(via)
        self.assertIn('via_without_interlayer_role',{f['kind'] for f in self.findings()})

    def test_thermal_via_connecting_planes_is_not_redundant(self):
        via=k.PCB_VIA(self.board);via.SetNet(self.net('GND'))
        via.SetPosition(k.VECTOR2I(k.FromMM(2),k.FromMM(2)))
        via.SetWidth(k.FromMM(.65));via.SetDrill(k.FromMM(.3))
        via.SetLayerPair(k.F_Cu,k.B_Cu);via.SetViaType(k.VIATYPE_THROUGH);self.board.Add(via)
        for layer in (k.In1_Cu,k.In2_Cu):
            zone=k.ZONE(self.board);zone.SetLayer(layer);zone.SetNet(self.net('GND'))
            poly=zone.Outline();poly.NewOutline()
            for x,y in [(0,0),(5,0),(5,5),(0,5)]:poly.Append(k.FromMM(x),k.FromMM(y))
            zone.SetFilledPolysList(layer,poly);zone.SetIsFilled(True);self.board.Add(zone)
        self.assertNotIn('via_without_interlayer_role',{f['kind'] for f in self.findings()})

    def test_import_cannot_overwrite_existing_board(self):
        self.save()
        with self.assertRaisesRegex(ValueError,'new proposal'):finish(self.path,self.path)

    def test_export_refuses_inner_routing_without_changing_source(self):
        self.track((0,0),(5,0),layer=k.In2_Cu);self.save()
        before=self.path.read_bytes();output=self.path.with_suffix('.dsn')
        with self.assertRaisesRegex(ValueError,'inner-layer'):prepare(self.path,output)
        self.assertEqual(self.path.read_bytes(),before);self.assertFalse(output.exists())

    def test_import_rejects_router_using_inner_layer(self):
        self.save();session=self.path.with_suffix('.ses');session.write_text('(path In1.Cu 150 0 0 1000 0)')
        output=self.path.parent/'proposal'/self.path.name
        with self.assertRaisesRegex(RuntimeError,'reference plane'):finish(self.path,output,session)
        self.assertFalse(output.exists())


if __name__=='__main__':unittest.main()
