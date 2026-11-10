#!/usr/bin/env python3
"""Relink the retained Beta 1 build objects using an older libm at link time."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def sha256(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def link_arguments(arguments):
    """Keep the original link inputs and discard outputs in the original build."""
    result = []
    skip = False
    for arg in arguments:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
        elif arg != "-Xlinker" and not arg.startswith(("-Wl,-Map=", "--dependency-file=")):
            result.append(arg)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("beta1_appdir", type=Path)
    parser.add_argument("reference_libm", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--build-workspace", type=Path, default=Path.home())
    args = parser.parse_args()
    workspace = args.build_workspace.resolve()
    original = args.beta1_appdir.resolve() / "emulator/elf"
    output = args.output.resolve()
    if output == original:
        raise SystemExit("Choose an output folder outside the Beta 1 package")
    output.mkdir(parents=True, exist_ok=True)
    audit = workspace / "kyty-native-vmid-20261110/integration/reserved-vmid-20261110"
    native_build = workspace / "kyty-native-instructions-20261106/_Build/native-instructions"
    audio_build = workspace / "kyty-published-upstream-b4496cc-20261103/build-b4496-guard"
    legacy_build = workspace / "kyty-native-1007-384scan-20261029/_Build/linux-native384"
    recipes = {}
    for variant in ("native", "astro", "guard", "upscaling"):
        name = "kyty_emulator_" + variant
        command = (audit / "logs" / f"{name}-link.log").read_text().splitlines()[0]
        recipes[name] = (audio_build if variant == "guard" else native_build, shlex.split(command))
    commands = json.loads(subprocess.check_output(
        ["ninja", "-t", "compdb", "-x"], cwd=legacy_build, text=True
    ))
    legacy_link = next(command["command"] for command in commands if command["output"] == "kyty_emulator")
    recipes["kyty_emulator_fast_guard"] = (legacy_build, shlex.split(legacy_link.split(" && ")[1]))

    manifest = {"link_only_libm_sha256": sha256(args.reference_libm), "backends": {}}
    with tempfile.TemporaryDirectory(prefix="kyty-link-") as temp_dir:
        temp = Path(temp_dir)
        (temp / "libm.so").symlink_to(args.reference_libm.resolve())
        for name, (cwd, command) in recipes.items():
            command = link_arguments(command)
            baseline = temp / name
            subprocess.run([*command, "-o", str(baseline)], cwd=cwd, check=True)
            old_hash = sha256(original / name)
            if sha256(baseline) != old_hash:
                raise SystemExit(f"Original objects do not reproduce Beta 1 for {name}")
            dest = output / name
            subprocess.run([*command, f"-L{temp}", "-o", str(dest)], cwd=cwd, check=True)
            manifest["backends"][name] = {
                "method": "relinked original objects",
                "beta1_sha256": old_hash,
                "beta2_sha256": sha256(dest),
            }
            print(f"{name}: reproduced the Beta 1 hash and relinked with the older libm", flush=True)

    legacy = "kyty_emulator_fast"
    subprocess.run([
        sys.executable, str(Path(__file__).with_name("rebind-legacy-libm.py")),
        str(original / legacy), str(args.reference_libm), str(output / legacy),
    ], check=True)
    manifest["backends"][legacy] = {
        "method": "rebound seven imported libm symbols in the retained ELF",
        "beta1_sha256": sha256(original / legacy),
        "beta2_sha256": sha256(output / legacy),
    }
    (output / "LIBM-COMPATIBILITY.json").write_text(json.dumps(manifest, indent=2) + "\n")
