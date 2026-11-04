"""Regenerate the three deliberately synthetic, self-contained glTF 2.0 assets.
No Fusion geometry is approximated or extracted here. Run manually, not at build time.
"""
import json
import math
from pathlib import Path
import struct


def cube():
    positions, normals, indices = [], [], []
    for axis in range(3):
        for sign in (-1, 1):
            start = len(positions)
            n = [0, 0, 0]; n[axis] = sign
            for i in range(4):
                v = [0, 0, 0]; v[axis] = sign * .5
                v[(axis + 1) % 3] = .5 if i in (1, 2) else -.5
                v[(axis + 2) % 3] = .5 if i >= 2 else -.5
                positions.append(v); normals.append(n)
            order = (0, 1, 2, 0, 2, 3) if sign > 0 else (0, 2, 1, 0, 3, 2)
            indices.extend(start + i for i in order)
    return positions, normals, indices


def cylinder():
    positions, normals, indices = [], [], []
    for i in range(48):
        a, b = i * math.tau / 48, (i + 1) * math.tau / 48
        na, nb = (math.cos(a), math.sin(a), 0), (math.cos(b), math.sin(b), 0)
        p0, p1 = (na[0]*.5, na[1]*.5, -.5), (nb[0]*.5, nb[1]*.5, -.5)
        p2, p3 = (nb[0]*.5, nb[1]*.5, .5), (na[0]*.5, na[1]*.5, .5)
        for pos, normal in ((p0,na),(p1,nb),(p2,nb),(p0,na),(p2,nb),(p3,na),
                            ((0,0,.5),(0,0,1)),(p2,(0,0,1)),(p3,(0,0,1)),
                            ((0,0,-.5),(0,0,-1)),(p1,(0,0,-1)),(p0,(0,0,-1))):
            indices.append(len(positions)); positions.append(pos); normals.append(normal)
    return positions, normals, indices


def export(name, pieces):
    data = bytearray()
    document = {'asset': {'version': '2.0', 'generator': 'ProtoMate synthetic fixture generator'},
                'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [], 'meshes': [],
                'buffers': [], 'bufferViews': [], 'accessors': [], 'materials': []}
    def accessor(values, kind, component=5126):
        while len(data) % 4: data.append(0)
        offset = len(data)
        flat = values if kind == 'SCALAR' else [x for row in values for x in row]
        data.extend(struct.pack('<' + ('f' if component == 5126 else 'I') * len(flat), *flat))
        view = len(document['bufferViews'])
        document['bufferViews'].append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data)-offset})
        record = {'bufferView': view, 'componentType': component, 'count': len(values), 'type': kind}
        if kind == 'VEC3':
            record['min'] = [min(row[i] for row in values) for i in range(3)]
            record['max'] = [max(row[i] for row in values) for i in range(3)]
        index = len(document['accessors']); document['accessors'].append(record)
        return index
    for color in ([.16,.43,.64,1],[.48,.54,.60,1],[.09,.11,.14,1]):
        document['materials'].append({'pbrMetallicRoughness': {'baseColorFactor': color,
                                      'metallicFactor': 0, 'roughnessFactor': .7}, 'doubleSided': True})
    # Author in robot-local millimetres/Z-up, store a standards-compliant metre/Y-up root.
    document['nodes'].append({'name': 'DUMMY_' + name, 'scale': [.001]*3,
                              'rotation': [-math.sqrt(.5),0,0,math.sqrt(.5)],
                              'children': list(range(1, len(pieces)+1))})
    primitives = []
    for positions, normals, indices in (cube(), cylinder()):
        primitives.append({'attributes': {'POSITION': accessor(positions,'VEC3'),
                                          'NORMAL': accessor(normals,'VEC3')},
                           'indices': accessor(indices,'SCALAR',5125)})
    for shape in range(2):
        for material in range(3):
            document['meshes'].append({'primitives': [{**primitives[shape], 'material': material}]})
    for i, (shape, material, location, dimensions) in enumerate(pieces):
        document['nodes'].append({'name': f'dummy_{name}_{i}', 'mesh': shape*3+material,
                                  'translation': location, 'scale': dimensions})
    document['buffers'] = [{'byteLength': len(data)}]
    encoded = json.dumps(document, separators=(',',':')).encode()
    encoded += b' ' * (-len(encoded) % 4)
    data += b'\0' * (-len(data) % 4)
    content = struct.pack('<III',0x46546c67,2,12+8+len(encoded)+8+len(data))
    content += struct.pack('<II',len(encoded),0x4e4f534a)+encoded
    content += struct.pack('<II',len(data),0x004e4942)+data
    Path(__file__).with_name(name+'.glb').write_bytes(content)


if __name__ == '__main__':
    export('shoulder', [(0,0,[102.5,0,0],[205,38,28]),
                        (1,1,[0,0,-8],[80,80,40]), (1,1,[205,0,0],[64,64,40]),
                        (0,2,[85,0,18],[105,25,8])])
    export('forearm', [(0,0,[111.7,0,12],[223.4,32,24]),
                       (1,2,[0,0,20],[52,52,22]), (1,1,[223.4,0,12],[46,46,30]),
                       (0,1,[223.4,0,-40],[24,38,160]),
                       (0,2,[223.4,-23,-40],[9,9,160]),
                       (0,2,[223.4,23,-40],[9,9,160])])
    export('carriage', [(0,0,[0,0,32],[40,64,64]),
                        (0,1,[67.5,0,8],[135,26,16]),
                        (1,1,[135,0,35],[22,22,70]),
                        (1,2,[135,0,4],[6,6,8])])
