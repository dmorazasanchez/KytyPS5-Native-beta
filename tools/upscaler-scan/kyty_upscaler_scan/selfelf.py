"""Read-only PS5 SELF/ELF analysis view.

Uses each SELF segment record's own mapping (as Kyty's Elf64::LoadSegment does) and keeps file offsets,
ELF offsets and module-relative addresses (RVAs) apart. Imports are decoded the way Kyty's runtime linker
does (symbol "NID#lib#module", DT_OS_IMPORT_LIB ids); R_X86_64_RELATIVE addends are applied in an analysis
copy of the data so that pointer tables (shader records, name tables) can be followed.
"""
from __future__ import annotations

import bisect
import struct

PT_LOAD, PT_DYNAMIC = 1, 2
PT_OS_DYNLIBDATA, PT_OS_RELRO = 0x61000000, 0x61000010
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 1, 6, 7, 8

DT_NULL, DT_PLTRELSZ, DT_RELA, DT_RELASZ, DT_JMPREL = 0, 2, 7, 8, 23
DT_OS = {
    'IMPORT_LIB': 0x61000015, 'IMPORT_LIB_1': 0x61000049, 'NEEDED_MODULE': 0x6100000f,
    'NEEDED_MODULE_1': 0x61000045, 'JMPREL': 0x61000029, 'PLTRELSZ': 0x6100002d, 'RELA': 0x6100002f,
    'RELASZ': 0x61000031, 'STRTAB': 0x61000035, 'STRSZ': 0x61000037, 'SYMTAB': 0x61000039,
    'SYMTABSZ': 0x6100003f,
}
_ID_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'


class UnsupportedImage(ValueError):
    """The executable cannot be analysed faithfully (encrypted, compressed, truncated)."""


def encode_id(value: int) -> str:
    """Kyty's EncodeId64: library/module ids as they appear in symbol names."""
    if value < 0x40:
        return _ID_CHARS[value]
    if value < 0x1000:
        return _ID_CHARS[(value >> 6) & 0x3f] + _ID_CHARS[value & 0x3f]
    return _ID_CHARS[(value >> 12) & 0x3f] + _ID_CHARS[(value >> 6) & 0x3f] + _ID_CHARS[value & 0x3f]


class Segment:
    __slots__ = ('index', 'type', 'flags', 'offset', 'vaddr', 'filesz', 'memsz', 'data')

    def __init__(self, index, header, data):
        self.index = index
        self.type, self.flags, self.offset, self.vaddr, _paddr, self.filesz, self.memsz, _align = header
        self.data = data

    @property
    def executable(self):
        return self.type == PT_LOAD and self.flags & 1

    def __repr__(self):
        return f'Segment({self.index}, type={self.type:#x}, va={self.vaddr:#x}, size={self.filesz:#x})'


class Import:
    __slots__ = ('nid', 'library', 'library_version', 'module', 'got_rva', 'symbol', 'plt_rvas')

    def __init__(self, nid, library, library_version, module, got_rva, symbol):
        self.nid, self.library, self.library_version, self.module = nid, library, library_version, module
        self.got_rva, self.symbol, self.plt_rvas = got_rva, symbol, []

    def as_json(self):
        return {'nid': self.nid, 'library': self.library, 'library_version': self.library_version,
                'module': self.module, 'got_rva': hex(self.got_rva), 'plt_rvas': [hex(x) for x in self.plt_rvas]}


class SelfImage:
    def __init__(self, raw: bytes):
        self.raw = raw
        if len(raw) < 32:
            raise UnsupportedImage('truncated SELF header')
        magic = raw[:4]
        if magic == b'\x7fELF':
            embedded, records = 0, None  # a plain ELF (already decrypted dump)
        else:
            count = struct.unpack_from('<H', raw, 24)[0]
            embedded = 32 + count * 32
            records = [struct.unpack_from('<4Q', raw, 32 + i * 32) for i in range(count)]
        if raw[embedded:embedded + 4] != b'\x7fELF':
            raise UnsupportedImage('no plain ELF64 inside the SELF (encrypted or unsupported container)')
        if raw[embedded + 4:embedded + 6] != b'\x02\x01':
            raise UnsupportedImage('not a little-endian ELF64')
        phoff = struct.unpack_from('<Q', raw, embedded + 32)[0]
        phsize, phnum = struct.unpack_from('<HH', raw, embedded + 54)
        if phsize != 56:
            raise UnsupportedImage('unsupported program header size')
        self.phdrs = [struct.unpack_from('<II6Q', raw, embedded + phoff + i * phsize) for i in range(phnum)]
        self.segments: list[Segment] = []
        if records is None:
            for i, h in enumerate(self.phdrs):
                if h[5]:
                    self.segments.append(Segment(i, h, raw[h[2]:h[2] + h[5]]))
        else:
            for flags, start, size, expanded in records:
                if not flags & 0x800:
                    continue
                index = (flags >> 20) & 0xfff
                if index >= phnum:
                    raise UnsupportedImage('SELF record names a missing program header')
                if flags & 0x8 or size != expanded:
                    raise UnsupportedImage('compressed SELF segment')
                if flags & 0x2:
                    raise UnsupportedImage('encrypted SELF segment')
                if start + size > len(raw) or size != self.phdrs[index][5]:
                    raise UnsupportedImage('SELF/ELF segment size mismatch')
                self.segments.append(Segment(index, self.phdrs[index], raw[start:start + size]))
        self.loaded = sorted((s for s in self.segments if s.type in (PT_LOAD, PT_OS_RELRO)), key=lambda s: s.vaddr)
        self._starts = [s.vaddr for s in self.loaded]
        if not any(s.executable for s in self.loaded):
            raise UnsupportedImage('no executable segment')
        self._dynamic()
        self._relocations()

    # ---- address-space access (module RVAs) ----
    def segment_at(self, rva):
        i = bisect.bisect_right(self._starts, rva) - 1
        if i >= 0:
            s = self.loaded[i]
            if rva < s.vaddr + max(s.filesz, s.memsz):
                return s
        return None

    def read(self, rva, size, relocated=True):
        s = self.segment_at(rva)
        if s is None:
            raise ValueError(f'unmapped RVA {rva:#x}')
        o = rva - s.vaddr
        data = self._relocated.get(s.index, s.data) if relocated else s.data
        out = bytes(data[o:o + size])
        return out + b'\0' * (size - len(out))

    def u64(self, rva):
        return struct.unpack('<Q', self.read(rva, 8))[0]

    def u32(self, rva):
        return struct.unpack('<I', self.read(rva, 4))[0]

    def code_segments(self):
        return [s for s in self.loaded if s.executable]

    def data_segments(self):
        return [s for s in self.loaded if not s.executable]

    def is_code(self, rva):
        s = self.segment_at(rva)
        return s is not None and bool(s.executable)

    def cstring(self, rva, limit=256):
        b = self.read(rva, limit)
        return b.split(b'\0', 1)[0].decode('latin-1')

    # ---- dynamic linking ----
    def _dynamic(self):
        dyn_h = next((h for h in self.phdrs if h[0] == PT_DYNAMIC), None)
        if dyn_h is None:
            raise UnsupportedImage('no dynamic table')
        dyn_seg = next((s for s in self.segments if s.type == PT_DYNAMIC), None)
        if dyn_seg is not None:
            table = dyn_seg.data
        else:  # dynamic table inside a loaded segment
            table = self.read(dyn_h[3], dyn_h[5], relocated=False)
        self.dyn: dict[int, list[int]] = {}
        for o in range(0, len(table) - 15, 16):
            tag, val = struct.unpack_from('<qQ', table, o)
            if tag == DT_NULL:
                break
            self.dyn.setdefault(tag, []).append(val)

        def first(tag):
            v = self.dyn.get(tag)
            return v[0] if v else None

        lib_seg = next((s for s in self.segments if s.type == PT_OS_DYNLIBDATA), None)
        if lib_seg is not None and first(DT_OS['STRTAB']) is not None:
            # Older layout: tables addressed as offsets into the PT_OS_DYNLIBDATA segment.
            lib = lib_seg.data

            def table_bytes(off_tag, size_tag):
                off, size = first(off_tag), first(size_tag)
                return lib[off:off + size] if off is not None and size else b''

            self._strings = table_bytes(DT_OS['STRTAB'], DT_OS['STRSZ'])
            symbols = table_bytes(DT_OS['SYMTAB'], DT_OS['SYMTABSZ'])
            self._jmprel = table_bytes(DT_OS['JMPREL'], DT_OS['PLTRELSZ'])
            self._rela = table_bytes(DT_OS['RELA'], DT_OS['RELASZ'])
        else:
            # Newer layout: standard tags holding module addresses of tables in a loaded segment.
            def table_bytes(addr_tag, size):
                addr = first(addr_tag)
                return self.read(addr, size, relocated=False) if addr is not None and size else b''

            self._strings = table_bytes(5, first(10))                      # DT_STRTAB, DT_STRSZ
            symbols = table_bytes(6, first(DT_OS['SYMTABSZ']))            # DT_SYMTAB
            self._jmprel = table_bytes(DT_JMPREL, first(DT_PLTRELSZ))
            self._rela = table_bytes(DT_RELA, first(DT_RELASZ))
        if not self._strings or not symbols:
            raise UnsupportedImage('dynamic string/symbol tables missing')

        def name_at(off):
            end = self._strings.find(b'\0', off)
            return self._strings[off:end].decode('latin-1')

        self.libraries = {}
        for tag in (DT_OS['IMPORT_LIB'], DT_OS['IMPORT_LIB_1']):
            for v in self.dyn.get(tag, []):
                self.libraries[encode_id((v >> 48) & 0xffff)] = (name_at(v & 0xffffffff), (v >> 32) & 0xffff)
        self.modules = {}
        for tag in (DT_OS['NEEDED_MODULE'], DT_OS['NEEDED_MODULE_1']):
            for v in self.dyn.get(tag, []):
                self.modules[encode_id((v >> 48) & 0xffff)] = name_at(v & 0xffffffff)
        self.symbols = []
        for o in range(0, len(symbols) - 23, 24):
            st_name, info, _other, shndx, value, size = struct.unpack_from('<IBBHQQ', symbols, o)
            self.symbols.append((name_at(st_name), info, shndx, value))

    def _relocations(self):
        self.imports: dict[str, Import] = {}       # by NID (first library wins; NIDs are unique in practice)
        self.imports_by_got: dict[int, Import] = {}
        self.relative: dict[int, int] = {}       # RVA of a pointer -> RVA it points at
        relocated: dict[int, bytearray] = {}
        for table in (self._rela, self._jmprel):
            for o in range(0, len(table) - 23, 24):
                offset, info, addend = struct.unpack_from('<QQq', table, o)
                rtype, sym = info & 0xffffffff, info >> 32
                if rtype == R_X86_64_RELATIVE:
                    self.relative[offset] = addend & 0xffffffffffffffff
                    seg = self.segment_at(offset)
                    if seg is not None and offset - seg.vaddr + 8 <= len(seg.data):
                        buf = relocated.get(seg.index)
                        if buf is None:
                            buf = relocated[seg.index] = bytearray(seg.data)
                        struct.pack_into('<Q', buf, offset - seg.vaddr, addend & 0xffffffffffffffff)
                elif rtype in (R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT, R_X86_64_64) and sym < len(self.symbols):
                    text, _info, shndx, _value = self.symbols[sym]
                    if shndx != 0 or '#' not in text:
                        continue
                    parts = text.split('#')
                    nid = parts[0]
                    lib_name, lib_ver = self.libraries.get(parts[1] if len(parts) > 1 else '', ('?', 0))
                    module = self.modules.get(parts[2] if len(parts) > 2 else '', '?')
                    imp = Import(nid, lib_name, lib_ver, module, offset, text)
                    self.imports.setdefault(nid, imp)
                    self.imports_by_got[offset] = imp
        self._relocated = {k: bytes(v) for k, v in relocated.items()}
        self._find_plt()

    def _find_plt(self):
        """PLT stubs: `jmp [rip+disp32]` (FF 25) whose target is an import's GOT slot."""
        self.plt: dict[int, Import] = {}
        for seg in self.code_segments():
            code = seg.data
            at = code.find(b'\xff\x25')
            while at >= 0:
                if at + 6 <= len(code):
                    target = seg.vaddr + at + 6 + struct.unpack_from('<i', code, at + 2)[0]
                    imp = self.imports_by_got.get(target)
                    if imp is not None:
                        imp.plt_rvas.append(seg.vaddr + at)
                        self.plt[seg.vaddr + at] = imp
                at = code.find(b'\xff\x25', at + 1)

    # ---- searches ----
    def find_bytes(self, needle: bytes, code=None):
        """All RVAs of `needle` in loaded segments (code=True/False to restrict)."""
        out = []
        for seg in self.loaded:
            if code is not None and bool(seg.executable) != code:
                continue
            at = seg.data.find(needle)
            while at >= 0:
                out.append(seg.vaddr + at)
                at = seg.data.find(needle, at + 1)
        return out

    def pointers_to(self, target_rva):
        """RVAs holding a RELATIVE relocation whose addend is `target_rva`."""
        if not hasattr(self, '_rel_index'):
            idx: dict[int, list[int]] = {}
            for where, to in self.relative.items():
                idx.setdefault(to, []).append(where)
            self._rel_index = idx
        return sorted(self._rel_index.get(target_rva, []))

    def _rel32_index(self):
        """Every byte position that would be a `call rel32` (E8) or `jmp rel32` (E9), by target. Candidates only:
        a position may be inside another instruction; callers confirm with instruction-aligned disassembly."""
        if hasattr(self, '_rel32'):
            return self._rel32
        index: dict[int, list[tuple[int, int]]] = {}
        try:
            import numpy as np
        except ImportError:  # pure-Python fallback (slower, same result)
            np = None
        for seg in self.code_segments():
            code = seg.data
            if np is not None:
                buf = np.frombuffer(code, dtype=np.uint8)
                if len(buf) < 5:
                    continue
                pos = np.nonzero((buf[:-4] == 0xE8) | (buf[:-4] == 0xE9))[0]
                rel = (buf[pos + 1].astype(np.int64) | (buf[pos + 2].astype(np.int64) << 8) |
                       (buf[pos + 3].astype(np.int64) << 16) | (buf[pos + 4].astype(np.int64) << 24))
                rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
                tgt = seg.vaddr + pos + 5 + rel
                lo, hi = self.loaded[0].vaddr, self.loaded[-1].vaddr + self.loaded[-1].memsz
                keep = (tgt >= lo) & (tgt < hi)
                for p, t, op in zip(pos[keep].tolist(), tgt[keep].tolist(), buf[pos[keep]].tolist()):
                    index.setdefault(t, []).append((seg.vaddr + p, op))
                # Jcc rel32 (0F 80..8F): conditional tail calls, recorded as op 0x0F at the 0F byte.
                pos = np.nonzero((buf[:-5] == 0x0F) & ((buf[1:-4] & 0xF0) == 0x80))[0]
                rel = (buf[pos + 2].astype(np.int64) | (buf[pos + 3].astype(np.int64) << 8) |
                       (buf[pos + 4].astype(np.int64) << 16) | (buf[pos + 5].astype(np.int64) << 24))
                rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
                tgt = seg.vaddr + pos + 6 + rel
                keep = (tgt >= lo) & (tgt < hi)
                for p, t in zip(pos[keep].tolist(), tgt[keep].tolist()):
                    index.setdefault(t, []).append((seg.vaddr + p, 0x0F))
            else:
                for op in (0xE8, 0xE9):
                    at = code.find(bytes([op]))
                    while at >= 0:
                        if at + 5 <= len(code):
                            t = seg.vaddr + at + 5 + struct.unpack_from('<i', code, at + 1)[0]
                            index.setdefault(t, []).append((seg.vaddr + at, op))
                        at = code.find(bytes([op]), at + 1)
                at = code.find(b'\x0f')
                while at >= 0:
                    if at + 6 <= len(code) and code[at + 1] & 0xF0 == 0x80:
                        t = seg.vaddr + at + 6 + struct.unpack_from('<i', code, at + 2)[0]
                        index.setdefault(t, []).append((seg.vaddr + at, 0x0F))
                    at = code.find(b'\x0f', at + 1)
        self._rel32 = index
        return index

    def rip_lea_refs(self, target_rva):
        """Candidate `lea r64, [rip+disp32]` (REX.W 8D /r, mod=00 rm=101) sites whose operand is `target_rva`."""
        if not hasattr(self, '_lea_index'):
            index: dict[int, list[int]] = {}
            import numpy as np
            for seg in self.code_segments():
                b = np.frombuffer(seg.data, dtype=np.uint8)
                if len(b) < 7:
                    continue
                pos = np.nonzero(((b[:-6] & 0xFB) == 0x48) & (b[1:-5] == 0x8D) & ((b[2:-4] & 0xC7) == 0x05))[0]
                rel = (b[pos + 3].astype(np.int64) | (b[pos + 4].astype(np.int64) << 8) |
                       (b[pos + 5].astype(np.int64) << 16) | (b[pos + 6].astype(np.int64) << 24))
                rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
                for p, t in zip(pos.tolist(), (seg.vaddr + pos + 7 + rel).tolist()):
                    index.setdefault(t, []).append(seg.vaddr + p)
            self._lea_index = index
        return sorted(self._lea_index.get(target_rva, []))

    def rel32_callers(self, target_rva, calls=True, jumps=True):
        """Candidate `call rel32` / `jmp rel32` / `jcc rel32` sites whose target is `target_rva`."""
        want = ({0xE8} if calls else set()) | ({0xE9, 0x0F} if jumps else set())
        return sorted(p for p, op in self._rel32_index().get(target_rva, []) if op in want)
