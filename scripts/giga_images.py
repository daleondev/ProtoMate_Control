#!/usr/bin/env python3
"""Produce a bounded, little-endian image-pair manifest from linked ELF32 files.

Only allocated file data in the two application partitions is programmed.
RAM initialization records are explicit; NOLOAD heaps, stacks and ICC are never
cleared by the application loader. No third-party Python packages are required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

MAGIC = 0x41474947
VERSION = 1
MAX_SEGMENTS = 16
MANIFEST_SIZE = 624
ZERO_SECTIONS = {'.bss', '.tbss', '.dtcm_bss', '.StepDmaSection'}


class Elf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:7] != b'\x7fELF\x01\x01\x01':
            raise ValueError('Expected little-endian ELF32')
        header = struct.unpack_from('<HHIIIIIHHHHHH', self.data, 16)
        if header[0] != 2 or header[1] != 40:
            raise ValueError('Expected linked Arm executable')
        self.entry, phoff, shoff = header[3:6]
        phsize, phnum, shsize, shnum, names_index = header[8:13]
        self.loads = [struct.unpack_from('<8I', self.data, phoff + i * phsize)
                      for i in range(phnum)]
        self.loads = [p for p in self.loads if p[0] == 1]
        raw = [struct.unpack_from('<10I', self.data, shoff + i * shsize)
               for i in range(shnum)]
        names = raw[names_index]
        table = self.data[names[4]:names[4] + names[5]]
        self.sections = [(table[s[0]:].split(b'\0')[0].decode(), s) for s in raw]

    def load_address(self, section):
        _, kind, _, address, offset, size, *_ = section
        if kind == 8:
            return None
        for _, start, vma, lma, file_size, *_ in self.loads:
            if start <= offset and offset + size <= start + file_size and vma <= address:
                if offset - start == address - vma:
                    return lma + address - vma
        raise ValueError(f'Allocated section at {address:#x} lacks a load segment')


def destination(core, address, length, ram_debug):
    regions = ([(0x10000000, 128 << 10), (0x30020000, 152 << 10)] +
               ([(0xC0400000, 2 << 20)] if ram_debug else [])) if core == 4 else [
                   (0, 64 << 10), (0x20000000, 112 << 10), (0x24000000, 256 << 10)]
    return any(start <= address and address + length <= start + size for start, size in regions)


def image(path, core):
    elf = Elf(path)
    base, limit = (0x90100000, 3 << 20) if core == 4 else (0x90400000, 4 << 20)
    vectors, stack = (0x10000000, 0x30048000) if core == 4 else (0x20000000, 0x20020000)
    ram_debug = core == 4 and 0xC0400000 <= elf.entry < 0xC0600000
    chunks, records = [], []
    vector_contents = None
    for name, s in elf.sections:
        _, kind, flags, address, offset, size, *_ = s
        if not (flags & 2) or not size:
            continue
        if kind == 8:
            if name in ZERO_SECTIONS:
                records.append((0, address, size, 2))
            continue
        load = elf.load_address(s)
        if not base <= load or load + size > base + limit:
            raise ValueError(f'{name}: load address outside application partition')
        contents = elf.data[offset:offset + size]
        if len(contents) != size:
            raise ValueError(f'{name}: truncated ELF')
        chunks.append((load, contents))
        if address != load:
            records.append((load, address, size, 1))
        if name == '.isr_vector':
            if address != vectors:
                raise ValueError('Unexpected vector-table address')
            vector_contents = contents
    if not vector_contents or struct.unpack_from('<II', vector_contents) != (stack, elf.entry):
        raise ValueError('Vector table does not match stack/entry')
    if not elf.entry & 1:
        raise ValueError('Entry point must use Thumb state')
    records.sort(key=lambda r: r[1])
    merged = []
    for load, run, size, flags in records:
        if not destination(core, run, size, ram_debug):
            raise ValueError(f'RAM section outside ownership: {run:#x}+{size}')
        if merged:
            previous = merged[-1]
            if previous[1] + previous[2] > run:
                raise ValueError('Overlapping RAM sections')
            if previous[1] + previous[2] == run and previous[3] == flags and (
                    flags == 2 or previous[0] + previous[2] == load):
                merged[-1] = (previous[0], previous[1], previous[2] + size, flags)
                continue
        merged.append((load, run, size, flags))
    if not 0 < len(merged) <= MAX_SEGMENTS:
        raise ValueError('Too many initialization segments')
    end = max(address + len(data) for address, data in chunks)
    payload = bytearray(b'\xff' * (end - base))
    previous_end = base
    for address, data in sorted(chunks):
        if address < previous_end:
            raise ValueError('Overlapping flash sections')
        payload[address - base:address - base + len(data)] = data
        previous_end = address + len(data)
    metadata = dict(core=core, storage=base, size=len(payload), crc=zlib.crc32(payload),
                    entry=elf.entry, vectors=vectors, stack=stack, flags=int(ram_debug),
                    segments=[dict(load=r[0], run=r[1], size=r[2], flags=r[3]) for r in merged])
    descriptor = struct.pack('<8I', base, len(payload), metadata['crc'], elf.entry,
                             vectors, stack, len(merged), int(ram_debug))
    descriptor += b''.join(struct.pack('<4I', *record) for record in merged)
    descriptor += bytes(16 * (MAX_SEGMENTS - len(merged)))
    return bytes(payload), descriptor, metadata


def pair(m4, m7):
    io, io_descriptor, io_meta = image(m4, 4)
    ui, ui_descriptor, ui_meta = image(m7, 7)
    pair_id = hashlib.sha256(io + ui).digest()
    def tag(payload, descriptor, metadata):
        payload += pair_id
        metadata['size'] = len(payload)
        metadata['crc'] = zlib.crc32(payload)
        descriptor = descriptor[:4] + struct.pack('<II', len(payload), metadata['crc']) + descriptor[12:]
        return payload, descriptor
    io, io_descriptor = tag(io, io_descriptor, io_meta)
    ui, ui_descriptor = tag(ui, ui_descriptor, ui_meta)
    if len(io) > 3 << 20 or len(ui) > 4 << 20:
        raise ValueError('Image pair footer exceeds partition')
    body = pair_id + io_descriptor + ui_descriptor
    manifest = struct.pack('<4I', MAGIC, VERSION, MANIFEST_SIZE, zlib.crc32(body)) + body
    assert len(manifest) == MANIFEST_SIZE
    return io, ui, manifest, dict(format=VERSION, pair_id=pair_id.hex(), images=[io_meta, ui_meta])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--m4', required=True, type=Path)
    parser.add_argument('--m7', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    io, ui, manifest, metadata = pair(args.m4, args.m7)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in [('io.bin', io), ('ui.bin', ui), ('manifest.bin', manifest)]:
        (args.output / name).write_bytes(data)
    (args.output / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Image pair {metadata["pair_id"][:16]}: M4 {len(io):,} B, M7 {len(ui):,} B')


if __name__ == '__main__':
    main()
