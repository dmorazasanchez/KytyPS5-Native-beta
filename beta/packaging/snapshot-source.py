#!/usr/bin/env python3
from pathlib import Path
import io, json, shutil, subprocess, tarfile
base = Path(__file__).resolve().parent.parent
home = Path('/home/david')
src = base / 'work/source'
native = home / 'kyty-native-vmid-20261110'
archive = subprocess.check_output(['git','-C',str(native),'archive','HEAD'])
with tarfile.open(fileobj=io.BytesIO(archive)) as t: t.extractall(src / 'emulator', filter='data')
def cp(a,b):
    if a.is_dir():
        shutil.copytree(a,b,dirs_exist_ok=True,symlinks=True,ignore=shutil.ignore_patterns('.git','__pycache__','*.o','*.a','*.so','*.dll','*.exe','*.pyc'))
    else:
        b.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(a,b)
# The native worktree uses submodule gitlinks; include checked-out source for those pins.
cp(home / 'kyty-native-instructions-20261106/3rdparty', src / 'emulator/3rdparty')
cp(native / '3rdparty/fidelityfx', src / 'emulator/3rdparty/fidelityfx')
audit = native / 'integration/reserved-vmid-20261110'
for name in ('astro-source','guard-source'):
    cp(audit/name, src/'variant-overlays'/name)
for name in ('README.md','build.py','build-guard.py','build-manifest.json','guard-build-manifest.json','inherited-source-changes.patch','inherited-source-files.json','pass-capture-disabled.cpp','native-vmid.patch'):
    cp(audit/name, src/'integration-records'/name)
cp(native/'integration/recovery-cheats-audit-20261110/scream-source.patch',src/'integration-records/scream-source.patch')
cp(native/'integration/recovery-cheats-audit-20261110/installed.json',src/'integration-records/installed.json')
cp(base/'packaging',src/'packaging')
pins = subprocess.check_output(['git','-C',str(native),'submodule','status'],text=True)
(src/'SUBMODULE-PINS.txt').write_text(pins)
build = home / 'kyty-native-instructions-20261106/_Build/native-instructions'
for name in ('CMakeCache.txt','build.ninja'):
    if (build/name).exists(): cp(build/name,src/'integration-records'/name)
cp(base/'downloads/runtimes.json',src/'runtime-downloads.json')
cp(home/'kyty-native-upscaling-20261108/experiments/native-upscaling/deps/ffx-2.4.0',src/'hlx-helper/deps/ffx-2.4.0')
cp(home/'kyty-native-upscaling-20261108/experiments/native-upscaling/deps/vulkan-headers',src/'hlx-helper/deps/vulkan-headers')
cp(home/'kyty-fsr3-framegen-20261107/experiments/hlx-fsr4/deps/HelixSR-Scaler/external/nvngx_dlss_sdk',src/'hlx-helper/deps/nvngx_dlss_sdk')
helper_build = src/'hlx-helper/build.sh'
if helper_build.exists():
    text = helper_build.read_text()
    text = text.replace('H=../deps/ffx-2.4.0','H=deps/ffx-2.4.0').replace('-I../deps/vulkan-headers','-Ideps/vulkan-headers')
    import re
    text = re.sub(r'N=/home/david/[^\n]+','N=deps/nvngx_dlss_sdk',text)
    helper_build.write_text(text)
(src/'README.md').write_text('''# KytyPS5 beta source snapshot

`launcher/` is the exact staged launcher source used for this beta. Build with
.NET SDK 10 (`dotnet publish src/KytyPS5.Launcher/KytyPS5.Launcher.csproj -c Release`).
`packaging/` contains the complete release/installation/relocation scripts.

`emulator/` snapshots aa1dff5 with checked-out third-party source, including the
shared VMID and Scream recovery fixes. See its README for CMake build prerequisites.
`variant-overlays/` preserves the source overlays for the previously integrated
Astro and recovery/audio variants; `integration-records/` captures their build
recipes, source hashes, compile configuration and final binary hashes. Those
records retain original developer build paths and incremental link history;
they are provenance records, not a claim that an old object-cache path exists
on a recipient's machine. Runtime packages contain no references to those paths.

The current runtime binaries were packaged byte for byte, not rebuilt during
packaging. Legacy Fast rollback binaries retain their original behavior. The
launcher defaults to the current Upstream backend in this beta.

Portable upstream runtime URLs and verified hashes are in runtime-downloads.json.
Proton/Wine source: https://github.com/CachyOS/proton-cachyos (packaged version
cachyos-11.0-20261005-slr); original notices are in the binary package.
The package excludes Helix-generated neural weights/shaders and the proprietary
Lossless DLL. Each tester prepares/imports their own files.
''')
print('Source snapshot prepared')
