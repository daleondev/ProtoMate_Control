"""Create a complete, hash-bound visual inspection atlas; never auto-sign it.

Each tile must be opened and inspected at full resolution. Overview images are
navigation aids, not replacements for the tile review. Record observations with
--record; validate refuses missing coverage, changed images or unresolved geometry.
"""
import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PatchCollection
from matplotlib.patches import Polygon as PatchPolygon
from matplotlib.patches import Circle
from PIL import Image, ImageDraw
import pcbnew as k
from shapely.geometry import box

from audit_copper import polygon_set
from layout_policy import TILE_MM, TILE_OVERLAP_MM, PIXELS_PER_MM, sha256
from audit_workmanship import scan

k.SwigPyIterator.next = k.SwigPyIterator.__next__
LAYERS = [('F', k.F_Cu), ('In1', k.In1_Cu), ('In2', k.In2_Cu), ('B', k.B_Cu)]
EVIDENCE_ROLES = {'gerber-F', 'gerber-In1', 'gerber-In2', 'gerber-B',
                  'gerber-top-mask-silk', 'gerber-bottom-mask-silk',
                  'mechanical-top', 'mechanical-bottom'}


def load_manifest(path):
    return json.loads(Path(path).read_text())


def binding(board):
    board = Path(board).resolve()
    paths = set(board.parent.glob('*.kicad_*'))
    paths.update(p for p in (board.parent.parent/'libraries').rglob('*')
                 if p.is_file() and p.suffix in ('.kicad_mod', '.kicad_sym', '.step', '.stp'))
    return {str(p.resolve()): sha256(p) for p in sorted(paths)
            if p.suffix not in ('.kicad_prl', '.kicad_pcb~') and p.is_file()}


def exported_binding(paths):
    result = {}
    for path in paths:
        path = Path(path).resolve()
        if not path.exists():
            raise RuntimeError('Missing export input: '+str(path))
        for item in sorted(path.rglob('*')) if path.is_dir() else [path]:
            if item.is_file():
                result[str(item)] = sha256(item)
    if not result:
        raise RuntimeError('Bind the actual Gerber/ODB/drill inputs, not just rendered pictures')
    return result


def inspection_tools():
    root=Path(__file__).resolve().parent
    return {name:sha256(root/name) for name in
            ('review_atlas.py','audit_copper.py','audit_workmanship.py','layout_policy.py',
             'inspect_gerbers.py','review_layout.py')}


def save_manifest(path, manifest):
    Path(path).write_text(json.dumps(manifest, indent=2)+'\n')


def polygons_to_patches(polygons):
    # Zones have holes; draw interiors explicitly in the canvas background.
    for polygon in polygons:
        yield PatchPolygon(list(polygon.exterior.coords), closed=True), '#b6bcc5'
        for ring in polygon.interiors:
            yield PatchPolygon(list(ring.coords), closed=True), 'white'


def render(board, layer, filled, bounds, output):
    x0,y0,x1,y1 = bounds
    fig, ax = plt.subplots(figsize=((x1-x0)*PIXELS_PER_MM/100,
                                    (y1-y0)*PIXELS_PER_MM/100), dpi=100)
    fig.subplots_adjust(left=0, right=1, bottom=0, top=1)
    ax.set_xlim(x0,x1); ax.set_ylim(y1,y0); ax.set_aspect('equal'); ax.set_axis_off()
    if filled:
        for zone in board.Zones():
            if zone.GetIsRuleArea() or not zone.IsOnLayer(layer):
                continue
            for patch,color in polygons_to_patches(polygon_set(zone.GetFilledPolysList(layer))):
                patch.set_facecolor(color); patch.set_edgecolor('none'); ax.add_patch(patch)
    colors = {'GND':'#006f50', 'PWR_RTN':'#ba4b00'}
    for track in board.GetTracks():
        if isinstance(track,k.PCB_VIA):
            p=track.GetPosition();x,y=p.x/1e6,p.y/1e6
            if not (x0-1<x<x1+1 and y0-1<y<y1+1):continue
            ax.add_patch(Circle((x,y),track.GetWidth(layer)/2e6,
                color=colors.get(track.GetNetname(),'#2c467a'),linewidth=0))
            ax.add_patch(Circle((x,y),track.GetDrillValue()/2e6,color='white',linewidth=0))
        elif track.GetLayer()==layer:
            a,c=track.GetStart(),track.GetEnd()
            # Width expressed in points, derived from exact mm/pixel scale.
            ax.plot([a.x/1e6,c.x/1e6],[a.y/1e6,c.y/1e6],
                color=colors.get(track.GetNetname(),'#172d57'),
                linewidth=track.GetWidth()/1e6*PIXELS_PER_MM*72/100,
                solid_capstyle='round',solid_joinstyle='round')
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if not pad.IsOnLayer(layer):continue
            for polygon in polygon_set(pad.GetEffectivePolygon(layer)):
                ax.add_patch(PatchPolygon(list(polygon.exterior.coords),closed=True,
                    facecolor=colors.get(pad.GetNetname(),'#8b521b'),edgecolor='none'))
            if pad.GetDrillSize().x and pad.GetDrillSize().x==pad.GetDrillSize().y:
                p=pad.GetPosition();ax.add_patch(Circle((p.x/1e6,p.y/1e6),pad.GetDrillSize().x/2e6,color='white',linewidth=0))
    fig.savefig(output,dpi=100);plt.close(fig)


def generate(board_path, output, extra_images, export_sources):
    if output.exists():
        raise RuntimeError('Create a new atlas directory; never overwrite recorded inspections')
    output.mkdir(parents=True)
    board=k.LoadBoard(str(board_path))
    bbox=board.GetBoardEdgesBoundingBox()
    bounds=[bbox.GetX()/1e6,bbox.GetY()/1e6,bbox.GetRight()/1e6,bbox.GetBottom()/1e6]
    report=scan(board_path)
    (output/'geometry.json').write_text(json.dumps(report,indent=2)+'\n')
    manifest=dict(schema=2,source=binding(board_path),exports=exported_binding(export_sources),
        export_roots=[str(p.resolve()) for p in export_sources],tools=inspection_tools(),board=str(board_path.resolve()),
        tile_mm=TILE_MM,overlap_mm=TILE_OVERLAP_MM,pixels_per_mm=PIXELS_PER_MM,
        geometry_sha256=sha256(output/'geometry.json'),regions=[],dispositions={},
        review_result='PENDING',limitations='Local visual inspection, not physical qualification or AISLER CAM acceptance.')
    step=TILE_MM-TILE_OVERLAP_MM
    views=[(label,layer,filled) for label,layer in LAYERS
           for filled in ([False,True] if layer in (k.F_Cu,k.B_Cu) else [True])]
    # Check actual copper geometry, not only the bounding rectangle around the
    # GIGA service cutout, so empty regions do not dilute coverage statistics.
    for label,layer,filled in views:
        identifier=f'{label}-{"filled" if filled else "routes"}-overview'
        image=output/(identifier+'.png');render(board,layer,filled,bounds,image)
        manifest['regions'].append(dict(id=identifier,image=image.name,image_sha256=sha256(image),
            bounds_mm=bounds,kind='overview',layer=board.GetLayerName(layer),filled=filled,
            checks=['organisation','whole_board_return_paths','final_frozen_pass'],review=None))
        geometry=[]
        for f in board.GetFootprints():
            for p in f.Pads():
                if p.IsOnLayer(layer):geometry.extend(polygon_set(p.GetEffectivePolygon(layer)))
        for t in board.GetTracks():
            if t.IsOnLayer(layer):
                bb=t.GetBoundingBox();geometry.append(box(bb.GetX()/1e6,bb.GetY()/1e6,bb.GetRight()/1e6,bb.GetBottom()/1e6))
        if filled:
            for z in board.Zones():
                if not z.GetIsRuleArea() and z.IsOnLayer(layer):geometry.extend(polygon_set(z.GetFilledPolysList(layer)))
        for row in range(math.ceil((bounds[3]-bounds[1])/step)):
            for col in range(math.ceil((bounds[2]-bounds[0])/step)):
                x=bounds[0]+col*step;y=bounds[1]+row*step
                region=[x,y,min(x+TILE_MM,bounds[2]),min(y+TILE_MM,bounds[3])]
                if not any(g.intersects(box(*region)) for g in geometry):continue
                identifier=f'{label}-{"filled" if filled else "routes"}-r{row+1:02}-c{col+1:02}'
                image=output/(identifier+'.png');render(board,layer,filled,region,image)
                manifest['regions'].append(dict(id=identifier,image=image.name,image_sha256=sha256(image),
                    bounds_mm=region,kind='copper',layer=board.GetLayerName(layer),filled=filled,
                    checks=['route_geometry','pad_via_entries','redundancy','return_paths'],review=None))
    # Component-centred views complement (rather than replace) exhaustive tiles.
    for footprint in sorted(board.GetFootprints(),key=lambda f:f.GetReference()):
        reference=footprint.GetReference()
        if not reference.startswith(('J','U','F','Q')):continue
        # Some imported connector graphics retain a remote library origin.
        # Bound the inspection around physical contacts, not those graphics.
        pads=list(footprint.Pads())
        if not pads:continue
        boxes=[pad.GetBoundingBox() for pad in pads]
        contact_bounds=[min(bb.GetX() for bb in boxes)/1e6,
                        min(bb.GetY() for bb in boxes)/1e6,
                        max(bb.GetRight() for bb in boxes)/1e6,
                        max(bb.GetBottom() for bb in boxes)/1e6]
        # Buffer/regulator views include the associated termination and local
        # decoupling banks. Every remaining passive is covered by the tile grid.
        margin=4 if reference.startswith('U') else 1
        region=[max(bounds[0],contact_bounds[0]-margin),max(bounds[1],contact_bounds[1]-margin),
                min(bounds[2],contact_bounds[2]+margin),min(bounds[3],contact_bounds[3]+margin)]
        for label,layer in (('F',k.F_Cu),('B',k.B_Cu)):
            identifier=f'component-{reference}-{label}'
            image=output/(identifier+'.png');render(board,layer,False,region,image)
            manifest['regions'].append(dict(id=identifier,image=image.name,image_sha256=sha256(image),
                bounds_mm=region,kind='component',reference=reference,layer=board.GetLayerName(layer),
                checks=['fanout','entries_and_vias','soldering_access','local_return_and_power'],review=None))
    for role,path in extra_images.items():
        path=path.resolve()
        identifier='export-'+role
        if any(r['id']==identifier for r in manifest['regions']):raise RuntimeError('Duplicate evidence image ID')
        evidence=output/(identifier+path.suffix);shutil.copy2(path,evidence)
        manifest['regions'].append(dict(id=identifier,image=evidence.name,image_sha256=sha256(evidence),
            kind='export',role=role,checks=['copper','mask','silkscreen','mechanical'],review=None))
        if role.startswith('gerber-'):
            calibration=path.with_suffix('.json')
            if not calibration.exists():
                raise RuntimeError('Independent Gerber render needs a resolution/bounds sidecar: '+str(calibration))
            info=load_manifest(calibration)
            if info['pixels_per_mm']<PIXELS_PER_MM-1e-9 or info['image_sha256']!=sha256(path):
                raise RuntimeError('Uncalibrated or low-resolution Gerber evidence: '+str(path))
            im=Image.open(evidence)
            stride=round((TILE_MM-TILE_OVERLAP_MM)*info['pixels_per_mm'])
            extent=round(TILE_MM*info['pixels_per_mm'])
            for row,y in enumerate(range(0,im.height,stride),1):
                for col,x in enumerate(range(0,im.width,stride),1):
                    tile_id=f'{identifier}-r{row:02}-c{col:02}'
                    tile=output/(tile_id+'.png')
                    im.crop((x,y,min(x+extent,im.width),min(y+extent,im.height))).save(tile)
                    manifest['regions'].append(dict(id=tile_id,image=tile.name,image_sha256=sha256(tile),
                        kind='export-detail',role=role,pixel_origin=[x,y],pixels_per_mm=info['pixels_per_mm'],
                        checks=['export_copper','mask_openings','silkscreen_clearance','connector_orientation'],review=None))
    inventory=[{key:value for key,value in region.items() if key!='review'} for region in manifest['regions']]
    (output/'coverage.json').write_text(json.dumps(inventory,indent=2)+'\n')
    manifest['coverage_sha256']=sha256(output/'coverage.json')
    save_manifest(output/'review.json',manifest)
    # Labelled contact sheet is only an index. All full-resolution images remain.
    thumbs=[]
    for region in manifest['regions']:
        p=Path(region['image']);p=p if p.is_absolute() else output/p
        im=Image.open(p).convert('RGB');im.thumbnail((320,300))
        tile=Image.new('RGB',(340,340),'white');tile.paste(im,((340-im.width)//2,25))
        ImageDraw.Draw(tile).text((5,5),region['id'],fill='black');thumbs.append(tile)
    cols=4
    sheet=Image.new('RGB',(cols*340,math.ceil(len(thumbs)/cols)*340),'#ddd')
    for i,im in enumerate(thumbs):sheet.paste(im,((i%cols)*340,(i//cols)*340))
    sheet.save(output/'index.png')
    print(f'{len(manifest["regions"])} regions require inspection: {output / "review.json"}')


def validate(path):
    path=Path(path);manifest=load_manifest(path)
    if manifest['source'] != binding(Path(manifest['board'])):
        raise RuntimeError('Board/rules changed after atlas generation; invalidate and regenerate review')
    if manifest.get('schema')!=2 or not manifest.get('exports'):
        raise RuntimeError('Missing exact export-input hashes')
    if manifest.get('tools')!=inspection_tools():
        raise RuntimeError('Inspection tooling changed; regenerate the atlas with the current checks')
    if manifest['exports']!=exported_binding(manifest['export_roots']):
        raise RuntimeError('Export inputs changed after inspection')
    geometry=path.parent/'geometry.json'
    if sha256(geometry)!=manifest['geometry_sha256']:raise RuntimeError('Geometry report changed')
    report=json.loads(geometry.read_text())
    coverage=path.parent/'coverage.json'
    if sha256(coverage)!=manifest['coverage_sha256']:raise RuntimeError('Coverage inventory changed')
    inventory=[{key:value for key,value in region.items() if key!='review'} for region in manifest['regions']]
    if inventory!=json.loads(coverage.read_text()):raise RuntimeError('Review coverage is incomplete or changed')
    if report['counts']['errors']:raise RuntimeError('Unresolved geometry errors')
    for region in manifest['regions']:
        image=Path(region['image']);image=image if image.is_absolute() else path.parent/image
        if sha256(image)!=region['image_sha256']:raise RuntimeError('Evidence image changed: '+region['id'])
        review=region['review']
        if not review or not review.get('inspector') or len(review.get('observations',''))<25:
            raise RuntimeError('Uninspected region: '+region['id'])
        if sorted(review.get('checks',[]))!=sorted(region['checks']):raise RuntimeError('Incomplete region checklist: '+region['id'])
    ids={r['id'] for r in manifest['regions']}
    for finding in report['findings']:
        disposition=manifest['dispositions'].get(finding['id'])
        if not disposition or len(disposition.get('reason',''))<25 or disposition.get('region') not in ids:
            raise RuntimeError('Unresolved geometry candidate: '+finding['id'])
        if disposition.get('decision')!='retain':
            raise RuntimeError('Corrected geometry requires regeneration, not a stale disposition')
    roles={r['role'] for r in manifest['regions'] if r['kind']=='export'}
    if roles!=EVIDENCE_ROLES:
        raise RuntimeError('Missing independent Gerber/mechanical views: '+str(EVIDENCE_ROLES-roles))
    return dict(result='PASS — recorded local visual inspection',
        manifest_sha256=sha256(path),regions=len(manifest['regions']),
        reviewed_geometry_candidates=len(report['findings']),source=manifest['source'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    make=sub.add_parser('generate');make.add_argument('board',type=Path);make.add_argument('output',type=Path)
    make.add_argument('--evidence',action='append',default=[],metavar='ROLE=IMAGE',
        help='Each of: '+', '.join(sorted(EVIDENCE_ROLES)))
    make.add_argument('--export-source',action='append',required=True,type=Path,
        help='Exact exported Gerber/drill directory or ODB archive; repeat as needed')
    check=sub.add_parser('validate');check.add_argument('manifest',type=Path)
    record=sub.add_parser('record');record.add_argument('manifest',type=Path);record.add_argument('region')
    record.add_argument('--inspector',required=True);record.add_argument('--observations',required=True)
    args=parser.parse_args()
    if args.command=='generate':
        evidence={role:Path(image) for role,image in (s.split('=',1) for s in args.evidence)}
        if not set(evidence)<=EVIDENCE_ROLES:raise RuntimeError('Unknown evidence role')
        generate(args.board,args.output,evidence,args.export_source)
    elif args.command=='validate':print(json.dumps(validate(args.manifest),indent=2))
    else:
        manifest=load_manifest(args.manifest)
        if manifest['source']!=binding(Path(manifest['board'])):raise RuntimeError('Stale review')
        region=next(r for r in manifest['regions'] if r['id']==args.region)
        if len(args.observations)<25:raise RuntimeError('Record specific inspection observations')
        region['review']=dict(inspector=args.inspector,observations=args.observations,checks=region['checks'])
        save_manifest(args.manifest,manifest)


if __name__=='__main__':main()
