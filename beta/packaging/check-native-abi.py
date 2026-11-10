#!/usr/bin/env python3
"""Check glibc versions for the emulator routes and bundled session tools."""

import argparse
from pathlib import Path
import re
import subprocess


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    args = parser.parse_args()
    baseline = (2, 38)
    backends = sorted((args.appdir / "emulator/elf").glob("kyty_emulator_*"))
    if not backends:
        raise SystemExit("No packaged emulator backends found")
    session = args.appdir / "system/session-runtime"
    binaries = backends + sorted((session / "bin").glob("*")) + sorted((session / "lib").glob("*.so*"))
    requirements = {}
    for backend in binaries:
        version_info = subprocess.check_output(
            ["readelf", "--wide", "--version-info", str(backend)], text=True
        )
        versions = {
            tuple(int(part) for part in match.split("."))
            for match in re.findall(r"Name: GLIBC_(\d+\.\d+(?:\.\d+)?)", version_info)
        }
        if not versions:
            if backend.parent == session / "lib":
                continue
            raise SystemExit(f"No glibc version information for {backend.name}")
        required = max(versions)
        requirements[backend.relative_to(args.appdir)] = required
        print(f"{backend.name}: GLIBC_{'.'.join(map(str, required))}")
    if any(version > baseline for version in requirements.values()):
        raise SystemExit("A binary requires a newer glibc than the 2.38 package baseline")
