#!/usr/bin/env python3
"""Snapshot the installed Kyty beta into a relocatable AppDir. Never modifies the live install."""
from pathlib import Path
import hashlib, json, os, re, shutil, subprocess, sys

BASE = Path(__file__).resolve().parent.parent
HOME = Path('/home/david')
LIVE = HOME / 'sharpemu-upscaling-20261108'
NATIVE = HOME / 'kyty-native-vmid-20261110'
OUT = BASE / 'work/KytyPS5-Beta.AppDir'
SOURCE = BASE / 'work/source'
TEMPLATES = BASE / 'packaging'
VERSION = '2026.11.10-beta1'

def cp(src, dst):
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.git'))
    else: shutil.copy2(src, dst)

def write(path, text, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    if executable: path.chmod(0o755)

def run(*args):
    subprocess.run(args, check=True)

def sha(path): return hashlib.file_digest(open(path, 'rb'), 'sha256').hexdigest()

def main():
    if OUT.exists(): shutil.rmtree(OUT)
    OUT.mkdir(parents=True, exist_ok=True)
    SOURCE.mkdir(parents=True, exist_ok=True)
    launcher = SOURCE / 'launcher'
    for name in ('Directory.Build.props', 'Directory.Packages.props', 'global.json', 'nuget.config', 'LICENSE.txt', 'README-KytyPS5.md', 'AUDIO-KytyPS5.md'):
        cp(LIVE / name, launcher / name)
    cp(LIVE / 'src/KytyPS5.Launcher', launcher / 'src/KytyPS5.Launcher')
    cp(LIVE / 'src/SharpEmu.HLE/FileSystem/ZArchive.cs', launcher / 'src/SharpEmu.HLE/FileSystem/ZArchive.cs')
    # Only the staged beta source changes. Existing desktop preferences/install stay intact.
    bridge = launcher / 'src/KytyPS5.Launcher/KytyBridge.cs'
    s = bridge.read_text().replace('"BC-250 Renderer", "fast", ["fast","upstream"]', '"BC-250 Renderer", "upstream", ["fast","upstream"]')
    s = s.replace('?? "/home/david/kyty-native-release-160251b-20261029/kyty_emulator"', '?? Path.Combine(AppContext.BaseDirectory, "../emulator/kyty_emulator_native")')
    marker = '        return result;\n    }\n\n    // Read-only compatibility import.'
    replacement = '''        // Beta ships opt-in, exact-version cheat plans in a writable per-user directory.
        var bundledPatches = Environment.GetEnvironmentVariable("KYTYPS5_PATCH_ROOT");
        if (string.IsNullOrEmpty(result.GetValueOrDefault("patch")) && gamePath is not null &&
            bundledPatches is not null && Directory.Exists(bundledPatches))
        {
            var match = Directory.EnumerateFiles(bundledPatches, "*.json")
                .FirstOrDefault(p => PatchMatchesGame(p, gamePath, titleId));
            if (match is not null) result["patch"] = match;
        }
        return result;
    }

    // Read-only compatibility import.'''
    if marker not in s: raise RuntimeError('Launcher patch association anchor changed')
    s = s.replace(marker, replacement)
    bridge.write_text(s)
    scanner_src = launcher / 'src/KytyPS5.Launcher/UpscalerProfiles.cs'
    scanner_src.write_text(scanner_src.read_text().replace('?? "/home/david/kyty-auto-upscaler-20261109/tools/upscaler-scan/kyty-upscaler-scan"', '?? Path.Combine(AppContext.BaseDirectory, "../tools/upscaler-scan/kyty-upscaler-scan")'))
    modal = launcher / 'src/KytyPS5.Launcher/ConsoleWindow.Modals.cs'
    modal.write_text(modal.read_text().replace('"/usr/local/libexec/ps5-minimal-control"', '(Environment.GetEnvironmentVariable("KYTYPS5_SESSION_CONTROL") ?? "/var/lib/kyty-beta-system/session/ps5-minimal-control")'))
    music = launcher / 'src/KytyPS5.Launcher/MusicLibrary.cs'
    music.write_text(music.read_text().replace('new ProcessStartInfo("/usr/bin/ffmpeg")', 'new ProcessStartInfo(Environment.GetEnvironmentVariable("KYTYPS5_FFMPEG") ?? "ffmpeg")'))
    run('dotnet', 'publish', str(launcher / 'src/KytyPS5.Launcher/KytyPS5.Launcher.csproj'), '-c', 'Release', '--self-contained', 'false', '-o', str(OUT / 'launcher'), '--nologo', '-v', 'minimal', '-p:NuGetPackageRoot=' + str(LIVE / '.packages') + '/', '-p:RestorePackagesPath=' + str(LIVE / '.packages'))
    print('Launcher published', flush=True)
    cp(BASE / 'downloads/dotnet', OUT / 'dotnet')
    binaries = {}
    by_hash = {}
    for f in sorted((NATIVE / 'release/20261110-recovery').glob('kyty_emulator_*')):
        digest = sha(f)
        elf = OUT / 'emulator/elf' / f.name
        if digest in by_hash:
            elf.parent.mkdir(parents=True, exist_ok=True)
            if elf.exists() or elf.is_symlink(): elf.unlink()
            elf.symlink_to(by_hash[digest])
        else:
            cp(f, elf)
            by_hash[digest] = f.name
        binaries[f.name] = {'sha256': digest, 'source': str(f)}
        write(OUT / 'emulator' / f.name, '#!/usr/bin/env bash\nset -euo pipefail\nr="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"\nexec "$r/runtime/run-elf" "$r/emulator/elf/' + f.name + '" "$@"\n', True)
    for name in ('kyty_emulator', 'kyty_emulator_demons1007_screamguard_6d3e578b'):
        dst = 'kyty_emulator_fast' if name == 'kyty_emulator' else 'kyty_emulator_fast_guard'
        f = HOME / 'kyty-native-release-160251b-20261029' / name
        cp(f, OUT / 'emulator/elf' / dst)
        binaries[dst] = {'sha256': sha(f), 'source': str(f)}
        write(OUT / 'emulator' / dst, '#!/usr/bin/env bash\nset -euo pipefail\nr="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"\nexec "$r/runtime/run-elf" "$r/emulator/elf/' + dst + '" "$@"\n', True)
    cp('/usr/local/bin/kyty-lodhw', OUT / 'system/kyty-lodhw')
    cp(HOME / 'kyty-upstream-clean-47857b4-20261104/release/assets', OUT / 'assets')
    (OUT / 'emulator/elf/assets').symlink_to('../../assets', target_is_directory=True)
    cp(NATIVE / 'tools/upscaler-scan', OUT / 'tools/upscaler-scan')
    shutil.rmtree(OUT / 'tools/upscaler-scan/tests', ignore_errors=True)
    for f in (HOME / 'kyty-native-instructions-20261106/release/_Patches/PPSA26344.json', HOME / 'kyty-upstream-clean-47857b4-20261104/release/_Patches/PPSA01341_01.007.000.json'):
        obj = json.loads(f.read_text(encoding='utf-8-sig'))
        for mod in obj.get('mods', []): mod['enabled'] = False
        write(OUT / 'patches' / (obj['id'] + '_' + obj['version'] + '.json'), json.dumps(obj, indent=2) + '\n')
    cp(LIVE / 'src/KytyPS5.Launcher/Assets/kytyps5.png', OUT / 'kytyps5.png')
    # Portable upstream builds keep the beta independent of CachyOS's rolling glibc.
    cp(BASE / 'downloads/python', OUT / 'python')
    cp(BASE / 'downloads/ffmpeg', OUT / 'runtime/ffmpeg-runtime')
    cp(BASE / 'downloads/binutils/usr/bin/x86_64-linux-gnu-objdump', OUT / 'runtime/objdump')
    seeds = ['/usr/lib/libstdc++.so.6', '/usr/lib/libgcc_s.so.1']
    libs = {}
    for seed in seeds:
        if not seed.startswith(str(OUT)): libs[Path(seed).name] = seed
        result = subprocess.run(['ldd', seed], text=True, capture_output=True)
        for line in result.stdout.splitlines():
            match = re.search(r'(?:=> )?(/\S+) \(', line)
            if match:
                lib = match.group(1)
                if Path(lib).name not in ('libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2'):
                    libs[Path(lib).name] = lib
    for name, path in sorted(libs.items()): cp(path, OUT / 'runtime/lib' / name)
    for f in (BASE / 'downloads/binutils/usr/lib/x86_64-linux-gnu').glob('*.so*'):
        cp(f.resolve(), OUT / 'runtime/lib' / f.name)
    sdl = next((BASE / 'downloads/sdl2/usr/lib/x86_64-linux-gnu').glob('libSDL2-2.0.so.0.*'))
    cp(sdl, OUT / 'runtime/lib/libSDL2-2.0.so.0')
    for name in ('libSDL2.so', 'libSDL2-2.0.so'):
        dst = OUT / 'runtime/lib' / name
        if not dst.exists(): dst.symlink_to('libSDL2-2.0.so.0')
    # Exact validated Wine 11.0 runtime, with no user's Wine prefix or caches.
    proton = HOME / '.local/share/Steam/compatibilitytools.d/proton-cachyos-11.0-20261005-slr-x86_64'
    cp(proton / 'files', OUT / 'proton/files')
    cp(proton / 'LICENSE', OUT / 'licenses/Proton-LICENSE')
    cp(proton / 'LICENSE.OFL', OUT / 'licenses/Proton-OFL')
    cp(proton / 'version', OUT / 'proton/version')
    helper = HOME / 'kyty-native-upscaling-20261108/experiments/native-upscaling/helper'
    for name in ('hlx_server.exe', 'hlx_server.exe.so', 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll'):
        cp(helper / name, OUT / 'upscalers/helper' / name)
    for name in ('hlx_server.cpp', 'hlx_dlss_ngx.h', 'build.sh'):
        cp(helper / name, SOURCE / 'hlx-helper' / name)
    # Ship only address/profile descriptions. The binary executable SHA binds them on the tester's PC.
    for f in (HOME / 'astro-helix-framegen-20261110/manual').glob('*.json'):
        if '.runtime.' in f.name: continue
        obj = json.loads(f.read_text())
        if 'game' in obj:
            obj['game'].pop('location', None)
            obj['game'].pop('stamp', None)
        write(OUT / 'upscalers/astro-templates' / f.name, json.dumps(obj, indent=2) + '\n')
    # Libraries are selected per application, without shipping the host's libc or Vulkan drivers.
    for f in ('run-elf', 'AppRun', 'runtime-selector.sh', 'prepare-runtime.py', 'doctor.py', 'install-desktop.py', 'install-system.py', 'import-upscalers.py', 'collect-report.py'):
        target = OUT / ('runtime/' + f if f == 'run-elf' else f)
        cp(TEMPLATES / f, target)
        target.chmod(0o755)
    for name, target in [('python3', '../python/bin/python3'), ('objdump', '../runtime/objdump')]:
        write(OUT / 'bin' / name, '#!/usr/bin/env bash\nset -euo pipefail\nr="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"\nexec "$r/runtime/run-elf" "$r/bin/' + target + '" "$@"\n', True)
    write(OUT / 'bin/ffmpeg', '#!/usr/bin/env bash\nset -euo pipefail\nr="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"\nexport LD_LIBRARY_PATH="$r/runtime/ffmpeg-runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexec "$r/runtime/ffmpeg-runtime/ffmpeg" "$@"\n', True)
    cp(TEMPLATES / 'README.md', OUT / 'README.md')
    write(OUT / 'VERSION', VERSION + '\n')
    write(OUT / 'kytyps5.desktop', '[Desktop Entry]\nType=Application\nName=KytyPS5 Beta\nComment=BC-250 native emulator beta\nExec=AppRun\nIcon=kytyps5\nCategories=Game;Emulator;\nTerminal=false\nStartupWMClass=KytyPS5\n')
    if not (OUT / '.DirIcon').exists(): (OUT / '.DirIcon').symlink_to('kytyps5.png')
    # Keep shipped sources/notices with the binary; source archive is produced separately.
    cp(LIVE / 'LICENSE.txt', OUT / 'licenses/Launcher-GPL-2.0.txt')
    cp(NATIVE / 'LICENSE', OUT / 'licenses/Emulator-GPL-2.0.txt')
    cp(NATIVE / 'LICENSES', OUT / 'licenses/emulator')
    for name in ('KytyPS5.deps.json',): cp(OUT / 'launcher' / name, OUT / 'licenses' / name)
    for package in ('libgcc','libstdc++'):
        cp(Path('/usr/share/licenses') / package, OUT / 'licenses' / package)
    write(OUT / 'licenses/HOST-PACKAGES.txt', subprocess.check_output(['pacman','-Qi','libgcc','libstdc++'], text=True))
    cp(TEMPLATES / 'THIRD-PARTY.md', OUT / 'licenses/THIRD-PARTY.md')
    cp(BASE / 'downloads/runtimes.json', OUT / 'licenses/runtime-downloads.json')
    cp(BASE / 'downloads/sdl2/usr/share/doc/libsdl2-2.0-0/copyright', OUT / 'licenses/SDL2-copyright')
    for f in (BASE / 'downloads/binutils/usr/share/doc').glob('*/copyright'):
        cp(f.resolve(), OUT / 'licenses' / (f.parent.name + '-copyright'))
    # Retain the original selector as evidence, then rebase only its paths/lifecycle in the beta copy.
    cp(LIVE / 'kytyps5-runtime.sh', SOURCE / 'installed-runtime-selector.sh')
    cp(HOME / 'ps5-minimal-session-20261109/files', SOURCE / 'gaming-mode-original')
    cp(HOME / 'ps5-minimal-session-20261109/files', OUT / 'system/session')
    manifest = {'version': VERSION, 'emulator_commit': subprocess.check_output(['git', '-C', str(NATIVE), 'rev-parse', 'HEAD'], text=True).strip(),
                'backends': binaries, 'host': Path('/etc/os-release').read_text(),
                'runtime': 'Host glibc >= 2.38; official .NET 10.0.15; portable CPython 3.14.10; host Vulkan ICD',
                'excluded': ['games', 'saves', 'personal settings', 'generated Helix networks', 'Lossless Scaling DLL'],
                'validation': 'See release validation report; Bazzite and SteamOS physical gameplay not yet tested.'}
    write(OUT / 'BUILD-MANIFEST.json', json.dumps(manifest, indent=2) + '\n')
    print('AppDir staged: ' + str(OUT), flush=True)

if __name__ == '__main__': main()
