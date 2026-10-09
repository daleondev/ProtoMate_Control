#!/usr/bin/env python3
"""Build this example's SII and C identity from its ESI (standard library only)."""
import argparse
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET


def number(text):
    return int(text[2:], 16) if text.lower().startswith('#x') else int(text)


def crc8(data):
    crc = 0xff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (7 if crc & 0x80 else 0)) & 0xff
    return crc


def generate(esi):
    root = ET.parse(esi).getroot()
    device = root.find('Descriptions/Devices/Device')
    dtype = device.find('Type')
    vendor = number(root.findtext('Vendor/Id'))
    product = number(dtype.get('ProductCode'))
    revision = number(dtype.get('RevisionNo'))
    size = number(device.findtext('Eeprom/ByteSize'))
    config = bytes.fromhex(device.findtext('Eeprom/ConfigData'))
    if size != 2048 or len(config) != 14 or config[0] != 0x80:
        raise ValueError('This example requires 2 KiB SII and 14-byte ordinary-SPI configuration')
    image = bytearray(128)
    image[:14] = config
    image[14] = crc8(config)
    struct.pack_into('<IIII', image, 0x10, vendor, product, revision, 0)
    sms = device.findall('Sm')
    expected_sm = [(0x1000, 128, 0x26), (0x1080, 128, 0x22), (0x1100, 4, 0x24), (0x1180, 4, 0x20)]
    sm_data = bytearray()
    for i, sm in enumerate(sms):
        actual = tuple(number(sm.get(a)) for a in ('StartAddress', 'DefaultSize', 'ControlByte'))
        if i >= 4 or actual != expected_sm[i] or sm.get('Enable') != '1':
            raise ValueError('SM layout must match ecat_options.h')
        sm_data += struct.pack('<HHBBBB', *actual, 0, 1, i + 1)
    if len(sms) != 4:
        raise ValueError('Four SyncManagers required')
    struct.pack_into('<HHHHH', image, 0x30, 0x1000, 128, 0x1080, 128, 0x0004)  # CoE only
    struct.pack_into('<HH', image, 0x7c, size // 128 - 1, 1)
    # SOEM uses the first SII string as its display name; General also points
    # to the same device-name string for masters that follow those indices.
    strings = [device.findtext('Name'), device.findtext('GroupType'), dtype.text, 'Command', 'Response', 'Value', 'Echo']

    def category(kind, data):
        if len(data) % 2:
            data += b'\0'
        image.extend(struct.pack('<HH', kind, len(data) // 2))
        image.extend(data)

    encoded = [s.encode('ascii') for s in strings]
    category(10, bytes([len(strings)]) + b''.join(bytes([len(s)]) + s for s in encoded))
    general = bytearray(32)
    general[:4] = bytes([2, 0, 3, 1])  # String indices: group, image, order, name
    general[5] = 0x13  # CoE SDO, SDO info, PDO upload; fixed mapping
    general[16] = 0x11  # Two MII ports
    category(30, general)
    category(40, bytes([1, 2]))  # FMMU outputs / inputs
    category(41, sm_data)
    for tag, category_id, mapping, sm_index, obj, name_idx, entry_idx in [
        ('RxPdo', 51, 0x1600, 2, 0x7000, 4, 6),
        ('TxPdo', 50, 0x1a00, 3, 0x6000, 5, 7),
    ]:
        pdo = device.find(tag)
        entries = pdo.findall('Entry')
        if (len(entries) != 1 or number(pdo.findtext('Index')) != mapping or
                number(pdo.get('Sm')) != sm_index):
            raise ValueError('PDO layout must match objectlist.c')
        entry = entries[0]
        if (number(entry.findtext('Index')) != obj or number(entry.findtext('SubIndex')) != 1 or
                number(entry.findtext('BitLen')) != 32 or entry.findtext('DataType') != 'UDINT'):
            raise ValueError('Expected one UDINT per PDO')
        data = struct.pack('<HBBBBH', mapping, 1, sm_index, 0, name_idx, 0)
        data += struct.pack('<HBBHBB', obj, 1, entry_idx, 7, 32, 0)
        category(category_id, data)
    image.extend(b'\xff\xff')
    if len(image) > size:
        raise ValueError('SII exceeds EEPROM size')
    image.extend(b'\xff' * (size - len(image)))
    identity = dict(vendor=vendor, product=product, revision=revision, name=strings[0])
    return bytes(image), identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('esi', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    image, identity = generate(args.esi)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'ProtoMateEcho.bin').write_bytes(image)
    header = '#pragma once\n#include <stdint.h>\n'
    for macro, key in [('EC_VENDOR_ID', 'vendor'), ('EC_PRODUCT_CODE', 'product'), ('EC_REVISION', 'revision')]:
        header += f'#define {macro} 0x{identity[key]:08x}U\n'
    header += f'#define EC_DEVICE_NAME {json.dumps(identity["name"])}\n#define EC_SII_SIZE {len(image)}\n'
    header += '#ifdef __cplusplus\nextern "C" {\n#endif\nextern const uint8_t ec_sii_image[EC_SII_SIZE];\n#ifdef __cplusplus\n}\n#endif\n'
    (args.output / 'device.h').write_text(header)
    rows = [', '.join(f'0x{b:02x}' for b in image[i:i+16]) for i in range(0, len(image), 16)]
    (args.output / 'sii.c').write_text('#include "device.h"\nconst uint8_t ec_sii_image[EC_SII_SIZE] = {\n' +
                                     ',\n'.join(rows) + '\n};\n')


if __name__ == '__main__':
    main()
