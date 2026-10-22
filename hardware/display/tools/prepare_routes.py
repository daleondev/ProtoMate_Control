#!/usr/bin/env python3
"""Export an outer-layer routing experiment; never edit the input PCB.

Power/thermal copper stays fixed. Signal routes can be rearranged. The resulting
router output is not an electrical or visual approval.
"""
import argparse
import re
from pathlib import Path
import pcbnew as k
from layout_policy import REFERENCE_LAYERS, SIGNAL_LAYERS

k.SwigPyIterator.next = k.SwigPyIterator.__next__


def prepare(source, output):
    if output.exists():
        raise ValueError('Use a new proposal filename')
    board = k.LoadBoard(str(source))
    if board.GetCopperLayerCount() != 4:
        raise ValueError('The common layout policy requires four layers')
    if any(not isinstance(t, k.PCB_VIA) and t.GetLayer() in REFERENCE_LAYERS
           for t in board.GetTracks()):
        raise ValueError('Remove inner-layer routing in a working copy first')
    if source.stem not in ('display-adapter', 'giga-adapter'):
        raise ValueError('Unknown board: connector-body keepouts must be explicit')
    for layer in REFERENCE_LAYERS:
        board.SetLayerType(layer, k.LT_POWER)
    detached = []
    for zone in list(board.Zones()):
        if not zone.GetIsRuleArea() and zone.GetNetname() == 'GND' and zone.GetLayer() in SIGNAL_LAYERS:
            board.Remove(zone)
            detached.append(zone)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not k.ExportSpecctraDSN(board, str(output)):
        raise RuntimeError('KiCad could not export DSN')
    data = output.read_text()
    data = data.replace('    (boundary\n', '''    (snap_angle fortyfive_degree)
    (autoroute_settings (autoroute on) (postroute on) (vias on)
      (layer_rule F.Cu (active on) (preferred_direction horizontal))
      (layer_rule In1.Cu (active off) (preferred_direction horizontal))
      (layer_rule In2.Cu (active off) (preferred_direction horizontal))
      (layer_rule B.Cu (active on) (preferred_direction vertical)))
    (boundary
''', 1)
    # Freerouting interprets KiCad's "type route" as user-fixed, not movable.
    # NORMAL is the actual unfixed state. Do not unlock supplies or return paths.
    def state(match):
        value = 'fix' if re.search(r'\(net (?:\+|GND\b|PWR_RTN\b)', match[0]) else 'normal'
        return re.sub(r'\(type (?:fix|route)\)', f'(type {value})', match[0])
    data = re.sub(r'\((wire|via)\b(?:(?!\(wire|\(via).)*?\(type (?:fix|route)\)\)',
                  state, data, flags=re.S)
    envelope = ('9950 -7000 45050 -1350' if source.stem == 'display-adapter'
                else '115000 -44050 120650 -8950')
    data = data.replace('    (boundary\n',
                        f'    (via_keepout "FFC body" (rect F.Cu {envelope}))\n    (boundary\n', 1)
    if source.stem == 'display-adapter':
        # DF23's underside contact bands must not cover exposed via annuli.
        # Coordinates are micrometres with inverted Y in the DSN export.
        for name, rectangle in [('DF23 upper contacts', '21000 -22200 34000 -21500'),
                                ('DF23 lower contacts', '21000 -24500 34000 -23800')]:
            data = data.replace('    (boundary\n',
                f'    (via_keepout "{name}" (rect B.Cu {rectangle}))\n    (boundary\n', 1)
    output.write_text(data)
    print(f'Routing experiment: {output}; source PCB unchanged')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
