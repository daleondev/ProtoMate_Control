#!/usr/bin/env python3
"""Check identity, CRC, category framing and fixed process layout of the example."""
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from generate_sii import generate, crc8


class Assets(unittest.TestCase):
    esi = Path(__file__).with_name('ProtoMateEcho.xml')

    def test_identity_and_boot_crc(self):
        image, identity = generate(self.esi)
        self.assertEqual(len(image), 2048)
        self.assertEqual(image[0], 0x80)
        self.assertEqual(image[14], crc8(image[:14]))
        self.assertEqual(image[15], 0)
        self.assertEqual(crc8(bytes.fromhex('800e00cc8813ff00000040c00000')), 0xde)
        self.assertEqual(struct.unpack_from('<IIII', image, 0x10),
                         (identity['vendor'], identity['product'], identity['revision'], 0))
        self.assertEqual(struct.unpack_from('<HH', image, 0x7c), (15, 1))

    def test_categories_mailboxes_and_pdos(self):
        image, identity = generate(self.esi)
        self.assertEqual(struct.unpack_from('<HHHHH', image, 0x30), (0x1000, 128, 0x1080, 128, 4))
        categories = {}
        pos = 128
        while struct.unpack_from('<H', image, pos)[0] != 0xffff:
            kind, words = struct.unpack_from('<HH', image, pos)
            self.assertNotIn(kind, categories)
            self.assertGreater(words, 0)
            self.assertLess(pos + 4 + words * 2, len(image))
            categories[kind] = image[pos+4:pos+4+words*2]
            pos += 4 + words * 2
        self.assertEqual(set(categories), {10, 30, 40, 41, 50, 51})
        self.assertEqual(categories[40], bytes([1, 2]))
        self.assertEqual(categories[30][5], 0x13)
        names = categories[10]
        self.assertEqual(names[2:2+names[1]].decode(), identity['name'])
        self.assertEqual(categories[30][3], 1)
        self.assertEqual(len(categories[41]), 32)
        for index, expected in enumerate([(0x1000,128,0x26), (0x1080,128,0x22), (0x1100,4,0x24), (0x1180,4,0x20)]):
            self.assertEqual(struct.unpack_from('<HHBBBB', categories[41], index*8), (*expected,0,1,index+1))
        for category, mapping, sm, obj in [(50, 0x1a00, 3, 0x6000), (51, 0x1600, 2, 0x7000)]:
            pdo = categories[category]
            self.assertEqual(struct.unpack_from('<HBB', pdo), (mapping, 1, sm))
            entry = struct.unpack_from('<HBBHBB', pdo, 8)
            self.assertEqual((entry[0], entry[1], entry[3], entry[4]), (obj, 1, 7, 32))
        self.assertEqual(image[pos:], b'\xff' * (len(image)-pos))

    def test_rejects_configuration_that_would_disagree_with_firmware(self):
        for selector, attribute, value in [('Sm', 'StartAddress', '#x1200'), ('RxPdo', 'Sm', '3')]:
            root = ET.parse(self.esi)
            root.find('Descriptions/Devices/Device/' + selector).set(attribute, value)
            with tempfile.TemporaryDirectory() as directory:
                filename = Path(directory) / 'bad.xml'
                root.write(filename)
                with self.assertRaises(ValueError):
                    generate(filename)


if __name__ == '__main__':
    unittest.main()
