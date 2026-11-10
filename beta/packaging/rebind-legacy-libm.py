#!/usr/bin/env python3
"""Rebind the retained Beta 1 Fast ELF to older libm symbol versions."""

import argparse
from pathlib import Path
from struct import pack_into, unpack_from

from elftools.elf.elffile import ELFFile


NEW_VERSIONS = {
    "GLIBC_2.43": {"sqrtf", "acosf", "atan2f", "asinf", "log10f"},
    "GLIBC_2.44": {"sinh", "cosh"},
}
OLD_VERSION = "GLIBC_2.2.5"
NEED = "<HHIII"
AUX = "<IHHII"


def rebind(source: Path, reference_libm: Path, output: Path):
    """Change symbol version metadata in a copy of the known legacy backend."""
    if output.resolve() == source.resolve():
        raise ValueError("Choose a different output path from the original ELF")
    data = bytearray(source.read_bytes())
    with reference_libm.open("rb") as file:
        reference = ELFFile(file)
        exported = {
            symbol.name: reference.get_section_by_name(".gnu.version").get_symbol(index).entry["ndx"]
            for index, symbol in enumerate(reference.get_section_by_name(".dynsym").iter_symbols())
            if symbol.entry["st_shndx"] != "SHN_UNDEF"
            and any(symbol.name in names for names in NEW_VERSIONS.values())
        }
        old_index = next(
            version.entry["vd_ndx"]
            for version, names in reference.get_section_by_name(".gnu.version_d").iter_versions()
            if next(names).name == OLD_VERSION
        )
        if set(exported) != set().union(*NEW_VERSIONS.values()) or any(
            index != old_index for index in exported.values()
        ):
            raise ValueError("Reference libm does not export the expected older symbols")

    with source.open("rb") as file:
        elf = ELFFile(file)
        if elf.elfclass != 64 or not elf.little_endian or elf["e_machine"] != "EM_X86_64":
            raise ValueError("Expected a little-endian x86-64 ELF")
        needed = elf.get_section_by_name(".gnu.version_r")
        versions = elf.get_section_by_name(".gnu.version")
        symbols = elf.get_section_by_name(".dynsym")
        strings = elf.get_section(needed["sh_link"])

        record = needed["sh_offset"]
        end = record + needed["sh_size"]
        while record < end:
            _, count, library_name, first_aux, next_record = unpack_from(NEED, data, record)
            if strings.get_string(library_name) == "libm.so.6":
                break
            if not next_record:
                raise ValueError("No libm version requirements found")
            record += next_record
        else:
            raise ValueError("No libm version requirements found")

        offset = record + first_aux
        offsets = {}
        for _ in range(count):
            _, _, index, name, next_aux = unpack_from(AUX, data, offset)
            offsets[strings.get_string(name)] = (offset, index)
            if not next_aux:
                break
            offset += next_aux
        if len(offsets) != count or not (NEW_VERSIONS.keys() | {OLD_VERSION}) <= offsets.keys():
            raise ValueError("Unexpected libm version table")
        replacements = {
            offsets[version][1]: names.copy() for version, names in NEW_VERSIONS.items()
        }
        old_index = offsets[OLD_VERSION][1]
        for index, symbol in enumerate(symbols.iter_symbols()):
            location = versions["sh_offset"] + index * 2
            raw_index, = unpack_from("<H", data, location)
            version_index = raw_index & 0x7FFF
            if version_index not in replacements:
                continue
            if symbol.entry["st_shndx"] != "SHN_UNDEF" or symbol.name not in replacements[version_index]:
                raise ValueError(f"Unexpected reference to a newer libm version: {symbol.name}")
            replacements[version_index].remove(symbol.name)
            pack_into("<H", data, location, (raw_index & 0x8000) | old_index)
        if any(replacements.values()):
            raise ValueError("Some expected imports were missing")

        kept = [offset for version, (offset, _) in offsets.items() if version not in NEW_VERSIONS]
        pack_into("<H", data, record + 2, len(kept))
        pack_into("<I", data, record + 8, kept[0] - record)
        for offset, following in zip(kept, kept[1:] + [0]):
            pack_into("<I", data, offset + 12, following - offset if following else 0)

    output.write_bytes(data)
    output.chmod(source.stat().st_mode & 0o777)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("reference_libm", type=Path)
    parser.add_argument("output", type=Path)
    options = parser.parse_args()
    rebind(options.source, options.reference_libm, options.output)
