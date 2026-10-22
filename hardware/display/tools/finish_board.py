#!/usr/bin/env python3
"""Import a routing proposal without overwriting authoritative placement or silk.

The result remains a proposal: fresh DRC, connectivity, geometry, return-path and
visual review are required before it may replace a project PCB.
"""
import argparse
import re
import shutil
from pathlib import Path
import pcbnew as k
from design import ROOT, BOARDS
from layout_policy import apply_ground_planes, protected_content

k.SwigPyIterator.next = k.SwigPyIterator.__next__


def finish(source, output, session=None, preserve_nets=()):
    source, output = Path(source).resolve(), Path(output).resolve()
    authoritative = {ROOT/name/(name+'.kicad_pcb') for name in BOARDS}
    if output in authoritative or output == source or output.exists():
        raise ValueError('Output must be a new proposal, never an existing or authoritative PCB')
    if source.stem != output.stem:
        raise ValueError('Keep the board basename; use a separate proposal directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    expected = protected_content(source)
    for suffix in ('.kicad_pro', '.kicad_dru'):
        if source.with_suffix(suffix).exists():
            shutil.copy2(source.with_suffix(suffix), output.with_suffix(suffix))
    for name in ('fp-lib-table', 'sym-lib-table'):
        if (source.parent/name).exists():
            shutil.copy2(source.parent/name, output.parent/name)
    board = k.LoadBoard(str(source))
    if session:
        data = Path(session).read_text()
        if re.search(r'\(path\s+"?In[12]\.Cu"?\s', data):
            raise RuntimeError('Router used a reserved reference plane; reject the whole proposal')
        # A SES can omit fixed wires. Preserve them explicitly; do not trust an
        # importer's lock handling or let it silently discard supply wiring.
        def fixed(item):
            return (item.GetNetname() in ('GND', 'PWR_RTN', *preserve_nets)
                    or item.GetNetname().startswith('+'))
        retained = []
        for item in board.GetTracks():
            if not fixed(item):
                continue
            via = isinstance(item, k.PCB_VIA)
            retained.append(dict(net=item.GetNetname(), via=via,
                start=(item.GetStart().x, item.GetStart().y),
                end=(item.GetEnd().x, item.GetEnd().y), layer=item.GetLayer(),
                width=item.GetWidth(k.F_Cu) if via else item.GetWidth(),
                drill=item.GetDrillValue() if via else None))
        if not k.ImportSpecctraSES(board, str(session)):
            raise RuntimeError('KiCad rejected the routing session')
        detached = []
        for item in list(board.GetTracks()):
            if fixed(item):
                board.Remove(item)
                detached.append(item)
        for original in retained:
            if original['via']:
                item = k.PCB_VIA(board)
                item.SetPosition(k.VECTOR2I(*original['start']))
                item.SetDrill(original['drill'])
                item.SetViaType(k.VIATYPE_THROUGH)
                item.SetLayerPair(k.F_Cu, k.B_Cu)
            else:
                item = k.PCB_TRACK(board)
                item.SetStart(k.VECTOR2I(*original['start']))
                item.SetEnd(k.VECTOR2I(*original['end']))
                item.SetLayer(original['layer'])
            item.SetWidth(original['width'])
            item.SetNet(board.FindNet(original['net']))
            board.Add(item)
    apply_ground_planes(board)
    for item in board.GetTracks():
        if isinstance(item, k.PCB_VIA):
            item.SetFrontTentingMode(k.TENTING_MODE_NOT_TENTED)
            item.SetBackTentingMode(k.TENTING_MODE_NOT_TENTED)
    board.BuildConnectivity()
    k.ZONE_FILLER(board).Fill(board.Zones())
    k.SaveBoard(str(output), board)
    if protected_content(output) != expected:
        raise RuntimeError('Proposal changed protected placement, footprints, mechanics or graphics')
    print(f'Unapproved routing proposal: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--session', type=Path)
    parser.add_argument('--preserve-net', action='append', default=[],
        help='Additional fixed source net omitted by the routing session')
    args = parser.parse_args()
    finish(args.source, args.output, args.session, args.preserve_net)
