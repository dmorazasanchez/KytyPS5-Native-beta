# Copied from the static-precompile tooling (KytyPS5-DemonsSouls-FrameGen-source/tools/local/static-precompile/gamefs.py),
# ZArchive reader only. Read-only: archives are never modified or extracted.
"""Read-only game file access over a directory or a ZArchive (.zar) dump.

The seed tooling predates ZArchive dumps; this module lets precompile.py treat a
.zar archive exactly like the emulator does (a mounted app0), without extracting
tens of gigabytes first. Only pure reads are supported — archives are
stream-mounted read-only, like the emulator.
"""

from __future__ import annotations

import io
import mmap
import struct
from compression import zstd
from pathlib import Path, PurePosixPath

_COMPRESSED_BLOCK_SIZE = 64 * 1024
_ENTRIES_PER_OFFSETRECORD = 16


def _u16be(b, o):
    return struct.unpack_from('>H', b, o)[0]


def _u32be(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _u64be(b, o):
    return struct.unpack_from('>Q', b, o)[0]


class Zar:
    """Minimal read-only reader for the ZArchive v1 container format."""

    def __init__(self, path):
        self.path = Path(path)
        self._fh = open(self.path, 'rb')
        self._mm = mmap.mmap(self._fh.fileno(), 0, access=mmap.ACCESS_READ)
        self._parse()

    def close(self):
        self._mm.close()
        self._fh.close()

    def _parse(self):
        mm = self._mm
        size = len(mm)
        if size <= 144:
            raise ValueError(f'{self.path}: not a ZArchive (too small)')
        version = _u32be(mm, size - 8)
        magic = _u32be(mm, size - 4)
        if magic != 0x169F52D6 or version != 0x61BF3A01:
            raise ValueError(f'{self.path}: bad ZArchive footer magic')
        total_size = _u64be(mm, size - 16)
        if total_size != size:
            raise ValueError(f'{self.path}: footer size mismatch {total_size} vs {size}')

        sections = []
        for i in range(6):
            sections.append((_u64be(mm, size - 144 + i * 16),
                             _u64be(mm, size - 144 + i * 16 + 8)))
        (self._comp_offset, self._comp_size), (off_offset, off_size), (names_offset, names_size), \
            (tree_offset, tree_size), _meta_dir, _meta_data = sections

        records = []
        for o in range(off_offset, off_offset + off_size, 8 + 2 * _ENTRIES_PER_OFFSETRECORD):
            base = _u64be(mm, o)
            sizes = [_u16be(mm, o + 8 + i * 2) for i in range(_ENTRIES_PER_OFFSETRECORD)]
            records.append((base, sizes))
        self._records = records
        self._names = mm[names_offset:names_offset + names_size]

        self._tree = []
        for o in range(tree_offset, tree_offset + tree_size, 16):
            name_type = _u32be(mm, o)
            a = _u32be(mm, o + 4)
            b = _u32be(mm, o + 8)
            c = _u32be(mm, o + 12)
            is_file = bool(name_type & 0x80000000)
            name_off = name_type & 0x7FFFFFFF
            if is_file:
                file_offset = a | ((c & 0xFFFF) << 32)
                file_size = b | (((c & 0xFFFF0000) << 16) & 0xFFFFFFFFFFFFFFFF)
                self._tree.append((is_file, name_off, file_offset, file_size, 0, 0))
            else:
                self._tree.append((is_file, name_off, 0, 0, a, b))
        self._block_cache = {}
        self._build_index()

    def _build_index(self):
        nodes = []  # (path, is_file)
        stack = []
        dir_children_start = self._tree[0][4]
        count = self._tree[0][5]

        def visit(idx, prefix):
            is_file, name_off, fo, fs, start, cnt = self._tree[idx]
            name = self._name(name_off)
            full = f'{prefix}/{name}' if prefix else name
            nodes.append((full, is_file, idx))
            if not is_file:
                for i in range(start, start + cnt):
                    visit(i, full)

        for i in range(dir_children_start, dir_children_start + count):
            visit(i, '')
        self._nodes = nodes
        self._bypath = {}
        for full, is_file, idx in nodes:
            self._bypath.setdefault(full.lower(), (idx, is_file, full))

    def paths(self):
        for full, is_file, _idx in self._nodes:
            yield full, is_file

    def lookup(self, path):
        """Return (node index, is_file, canonical path) or None."""
        key = str(path).strip('/').lower()
        return self._bypath.get(key)

    def _name(self, name_off):
        if name_off == 0x7FFFFFFF or name_off >= len(self._names):
            return ''
        first = self._names[name_off]
        length = first & 0x7F
        if first & 0x80:
            length |= self._names[name_off + 1] << 7
            name_off += 2
        else:
            name_off += 1
        if name_off + length > len(self._names):
            return ''
        return bytes(self._names[name_off:name_off + length]).decode('cp1252')

    # --- compressed block access ---

    def _block(self, block_index):
        # which record and offset within the record
        rec = block_index // _ENTRIES_PER_OFFSETRECORD
        idx = block_index % _ENTRIES_PER_OFFSETRECORD
        base, sizes = self._records[rec]
        offset = base + sum(s + 1 for s in sizes[:idx])
        comp = bytes(self._mm[self._comp_offset + offset:self._comp_offset + offset + sizes[idx] + 1])
        if block_index in self._block_cache:
            return self._block_cache[block_index]
        if len(comp) == _COMPRESSED_BLOCK_SIZE:
            raw = comp  # stored uncompressed
        else:
            raw = zstd.ZstdDecompressor().decompress(comp, max_length=_COMPRESSED_BLOCK_SIZE)
        if len(self._block_cache) > 64:
            self._block_cache.clear()
        self._block_cache[block_index] = raw
        return raw

    def read_node(self, index):
        is_file, _name_off, fo, fs, _s, _c = self._tree[index]
        if not is_file:
            raise IsADirectoryError(index)
        out = bytearray()
        offset = 0
        while offset < fs:
            b = (fo + offset) // _COMPRESSED_BLOCK_SIZE
            blk = self._block(b)
            start = (fo + offset) % _COMPRESSED_BLOCK_SIZE
            take = min(len(blk) - start, fs - offset)
            out += blk[start:start + take]
            offset += take
        return bytes(out)


