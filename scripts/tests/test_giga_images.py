#!/usr/bin/env python3
"""Host checks for the actual C boot validator and Python wire format."""
import ctypes
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('giga_images', ROOT / 'scripts/giga_images.py')
images = importlib.util.module_from_spec(spec)
spec.loader.exec_module(images)


def seal(words):
    data = bytearray(struct.pack('<156I', *words))
    struct.pack_into('<I', data, 12, zlib.crc32(data[16:]))
    return data


def manifest():
    words = [0] * 156
    words[:3] = [images.MAGIC, 1, 624]
    for core, start in [(4, 12), (7, 84)]:
        base, vector, stack = (0x90100000, 0x10000000, 0x30048000) if core == 4 else (0x90400000, 0x20000000, 0x20020000)
        words[start:start+8] = [base, 256, 0, base+65, vector, stack, 1, 0]
        words[start+8:start+12] = [base, vector, 64, 1]
    return words


class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='giga-validator-')
        library = Path(cls.directory.name) / 'format.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(ROOT/'platform/boot/giga/format.c'), '-o', str(library)], check=True)
        cls.validator = ctypes.CDLL(str(library))
        cls.validator.giga_manifest_valid.argtypes = [ctypes.c_void_p]
        cls.validator.giga_image_valid.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def valid(self, data):
        return bool(self.validator.giga_manifest_valid(ctypes.create_string_buffer(bytes(data))))

    def test_valid_pair_and_crc(self):
        data = seal(manifest())
        self.assertTrue(self.valid(data))
        for index in range(len(data)):
            corrupt = bytearray(data)
            corrupt[index] ^= 1
            self.assertFalse(self.valid(corrupt), index)

    def test_limits_and_ownership(self):
        # Fix CRC after each mutation so structural validation is exercised.
        changes = [(0,0), (1,2), (2,625), (12,0x90000000), (13,0), (13,0x300001),
                   (15,0x90100040), (16,0x10000400), (17,0x30020000), (18,0), (18,17),
                   (19,2), (20,0x90000000), (20,0xFFFFFFFF), (21,0x38000000),
                   (21,0x24040000), (21,0x30046000), (22,0xFFFFFFFF), (22,0), (23,0),
                   (84,0x90100000), (85,0x400001), (91,1), (93,0x2001C000),
                   (93,0x24040000), (93,0x3800F000)]
        for index, value in changes:
            words = manifest(); words[index] = value
            self.assertFalse(self.valid(seal(words)), (index, hex(value)))

    def test_overlap_and_debug_region(self):
        words = manifest()
        words[18] = 2
        words[24:28] = [0x90100080, 0x10000020, 32, 1]
        self.assertFalse(self.valid(seal(words)))
        words[25] = 0xC0400000
        words[15] = 0xC0400001
        self.assertFalse(self.valid(seal(words)))
        words[19] = 1
        self.assertTrue(self.valid(seal(words)))
        words[25] = 0xC0600000
        self.assertFalse(self.valid(seal(words)))

    def test_payload_crc_and_matching_pair_footer(self):
        pair = bytes(range(32))
        payload = bytes(range(64)) + pair
        descriptor = struct.pack('<8I', 0x90100000, len(payload), zlib.crc32(payload), 0, 0, 0, 0, 0) + bytes(256)
        valid = lambda p, length, ident=pair: bool(self.validator.giga_image_valid(
            ctypes.create_string_buffer(descriptor), ctypes.create_string_buffer(ident),
            ctypes.create_string_buffer(p), length))
        self.assertTrue(valid(payload, len(payload)))
        self.assertFalse(valid(payload, len(payload)-1))
        self.assertFalse(valid(payload, len(payload), pair[::-1]))
        self.assertFalse(valid(bytes([255])+payload[1:], len(payload)))

    def test_linked_images(self):
        # Self-contained ELF fixtures: tests must not race an independent
        # hardware linker or depend on artifacts from another build tree.
        def fixture(core):
            base, vector, stack = (0x90100000,0x10000000,0x30048000) if core == 4 else (0x90400000,0x20000000,0x20020000)
            data=bytearray(0x300)
            data[:16]=b'\x7fELF\x01\x01\x01'+bytes(9)
            struct.pack_into('<HHIIIIIHHHHHH',data,16,2,40,1,base+65,52,0x200,0,52,32,2,40,4,3)
            struct.pack_into('<8I',data,52,1,0x100,vector,base,64,64,4,4)
            struct.pack_into('<8I',data,84,1,0x140,base+64,base+64,64,64,5,4)
            struct.pack_into('<II',data,0x100,stack,base+65)
            data[0x140:0x142]=b'\x70\x47' # Thumb BX LR
            names=b'\0.isr_vector\0.text\0.shstrtab\0'
            data[0x180:0x180+len(names)]=names
            struct.pack_into('<10I',data,0x228,1,1,2,vector,0x100,64,0,0,4,0)
            struct.pack_into('<10I',data,0x250,13,1,6,base+64,0x140,64,0,0,4,0)
            struct.pack_into('<10I',data,0x278,19,3,0,0,0x180,len(names),0,0,1,0)
            path=Path(self.directory.name)/f'm{core}.elf';path.write_bytes(data)
            return path
        io,ui=fixture(4),fixture(7)
        m4, m7, data, metadata = images.pair(io, ui)
        self.assertTrue(self.valid(data))
        self.assertEqual(m4[-32:], m7[-32:])
        self.assertEqual(m4[-32:].hex(), metadata['pair_id'])
        for payload, info in zip([m4,m7], metadata['images']):
            self.assertEqual(zlib.crc32(payload), info['crc'])
            self.assertLessEqual(len(info['segments']), 16)
            self.assertTrue(all(s['run'] != 0x38000000 for s in info['segments']))

    def test_elf_rejects_wrong_input(self):
        for data in [b'', b'\x7fELF\x02\x01\x01'+bytes(100), b'\x7fELF\x01\x02\x01'+bytes(100)]:
            path = Path(self.directory.name)/'bad.elf'; path.write_bytes(data)
            with self.assertRaises(ValueError): images.Elf(path)


if __name__ == '__main__': unittest.main()
