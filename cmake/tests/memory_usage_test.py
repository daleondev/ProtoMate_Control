#!/usr/bin/env python3
"""Check load/run addresses, padding and reserved memory in the build report."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from memory_usage import memory_usage

MAP = """Memory Configuration
Name Origin Length Attributes
FLASH 0x08000000 0x1000 xr
RAM 0x20000000 0x100 xrw
STACK 0x20000100 0x100 rw
UNUSED 0x30000000 0x100 rw
*default* 0x00000000 0xffffffff
Linker script and memory map
"""
SECTIONS = """ 0 .text 00000030 08000000 08000000 00001000 2**2
 CONTENTS, ALLOC, LOAD, READONLY, CODE
 1 .data 00000010 20000000 08000040 00002000 2**2
 CONTENTS, ALLOC, LOAD, DATA
 2 .bss 00000010 20000020 08000050 00002010 2**2
 ALLOC
 3 .heap 00000040 20000030 08000050 00002030 2**2
 ALLOC
 4 .stack 00000100 20000100 20000100 00003000 2**2
 ALLOC
 5 .debug_info 00000400 00000000 00000000 00003000 2**0
 CONTENTS, READONLY, DEBUGGING
"""


class MemoryUsageTest(unittest.TestCase):
    def test_load_addresses_and_reserved_sections(self):
        self.assertEqual(memory_usage(MAP, SECTIONS), {
            "FLASH": (0x50, 0x1000), "RAM": (0x70, 0x100),
            "STACK": (0x100, 0x100), "UNUSED": (0, 0x100)})

    def test_missing_allocations_fail(self):
        with self.assertRaises(ValueError):
            memory_usage(MAP, "unrecognized objdump output")

    def test_out_of_region_fails(self):
        with self.assertRaises(ValueError):
            memory_usage(MAP, SECTIONS.replace("20000030", "40000000"))

    def test_overflow_fails(self):
        with self.assertRaises(ValueError):
            memory_usage(MAP, SECTIONS.replace("00000040 20000030", "00000100 20000030"))


if __name__ == "__main__":
    unittest.main()
