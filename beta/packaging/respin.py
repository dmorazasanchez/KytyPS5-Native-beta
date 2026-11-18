#!/usr/bin/env python3
"""Re-spin a release from the previous release's package: same emulator backends and bundled runtimes, with the
scripts and tools of this source tree laid over them.

  respin.py stage  PREVIOUS_APPDIR VERSION [NAME=ELF ...]
                                               work/KytyPS5-Beta.AppDir = previous package + this tree's files; each
                                               NAME=ELF replaces that emulator backend (and the names linked to it)
  respin.py finish PREVIOUS_APPIMAGE           manifests, FILES.sha256, AppImage (previous AppImage's runtime), tar.gz, zip
  respin.py minimal PREVIOUS_MINIMAL_TARBALL   the PS5 Minimal Mode download under the new version name
  respin.py source                             source snapshot of the committed tree (git archive HEAD)
  respin.py sums                               SHA256SUMS over dist/

The work directory is $KYTY_BETA_BASE (work/, dist/, validation/). Used for Beta 5 (2026.11.13: launch scripts and
upscaler scanner only) and Beta 6 (2026.11.15: the native and upscaler backends rebuilt from Superkeka's 2026-11-13
main, CMake build with KYTY_NATIVE_UPSCALER and KYTY_COMPAT_LIBM_DIR) and Beta 7 (2026.11.17: Wolverine fixes in
those two backends); `build.py` builds a package from scratch.
"""
import hashlib, json, os, shutil, subprocess, sys, tarfile
from pathlib import Path

repo = Path(__file__).resolve().parents[2]
base = Path(os.environ.get('KYTY_BETA_BASE') or Path(__file__).resolve().parent.parent)
root = base / 'work/KytyPS5-Beta.AppDir'
dist = base / 'dist'

# Source file -> place in the package. Everything else in the package is the previous release's, byte for byte.
OVERLAY = {
    'beta/packaging/selector-relocated.sh': 'selector-relocated.sh',
    'tools/upscaler-scan/kyty-render-preset': 'tools/upscaler-scan/kyty-render-preset',
    'tools/upscaler-scan/kyty_upscaler_scan/__main__.py': 'tools/upscaler-scan/kyty_upscaler_scan/__main__.py',
    'tools/upscaler-scan/kyty_upscaler_scan/scan.py': 'tools/upscaler-scan/kyty_upscaler_scan/scan.py',
    'tools/upscaler-scan/kyty_upscaler_scan/selfelf.py': 'tools/upscaler-scan/kyty_upscaler_scan/selfelf.py',
    'beta/packaging/doctor.py': 'doctor.py',
    'beta/packaging/prepare-system.py': 'prepare-system.py',
    'beta/packaging/package-README.md': 'README.md',
    'beta/packaging/AppRun': 'AppRun',
    'beta/packaging/prepare-runtime.py': 'prepare-runtime.py',
    'beta/packaging/session-hdr.py': 'session-hdr.py',
    'beta/gaming-mode-original/ps5-labwc': 'system/session/ps5-labwc',
    'beta/gaming-mode-original/ps5-minimal-compositor': 'system/session/ps5-minimal-compositor',
    'beta/gaming-mode-original/rc.xml': 'system/session/rc.xml',
    **{'beta/astro-templates/' + f.name: 'upscalers/astro-templates/' + f.name
       for f in sorted((Path(__file__).resolve().parents[1] / 'astro-templates').glob('*.json'))},
    **{'beta/title-profiles/' + f.name: 'upscalers/title-profiles/' + f.name
       for f in sorted((Path(__file__).resolve().parents[1] / 'title-profiles').glob('*')) if f.suffix in ('.json', '.py')},
    **{'beta/title-defaults/' + f.name: 'title-defaults/' + f.name
       for f in sorted((Path(__file__).resolve().parents[1] / 'title-defaults').glob('*.json'))},
}
CHANGES = [
    "Marvel's Wolverine: both main emulator backends restore the active shader for each guest compute queue "
    "at the start of its next command buffer. The player passed the opening-area crash with HLX FSR 4 and "
    "native FSR 3 frame generation enabled",
    "Marvel's Wolverine with frame generation keeps Fidelity at 30 fps: with frame generation on (native or Lossless), "
    "Fidelity renders at most 1920x1080 whatever the preset. A fresh install's new game ran Quality + HLX + frame "
    "generation at 20 fps: measured in that area, Quality holds 30 with HLX alone but falls to 20 once frame generation "
    "is on (at any frame-generation working size), 2176x1224 averages 25-28 and 1920x1080 29 (30 + 30 most of the "
    "time). Without frame generation every preset renders its own size (Quality: a steady 30 with HLX); Performance "
    "mode is unchanged",
    "Marvel's Wolverine: the 1440p and 1800p output choices set the presentation window while the game keeps its own internal output size",
    "The GPU fault report (gpu-fault.txt) is written on a fresh system too (its folder is created)",
    "The native and upscaler backends are rebuilt from that source; everything else is Beta 7's",
]


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def version():
    return (root / 'VERSION').read_text().strip()


def stage(previous, new_version, *replacements):
    previous = Path(previous)
    new_backends = {}
    helper = None
    for item in replacements:
        name, _, elf = item.partition('=')
        if name == 'hlx_server' and elf and Path(elf).is_file():
            helper = Path(elf)  # a built beta/hlx-helper (hlx_server.exe.so), replacing upscalers/helper's
            continue
        if not name.startswith('kyty_emulator_') or not elf or not Path(elf).is_file():
            raise SystemExit('replacement must be kyty_emulator_NAME=/path/to/elf or hlx_server=/path/to/hlx_server.exe.so: ' + item)
        new_backends[name] = Path(elf)
    old = json.loads((previous / 'BUILD-MANIFEST.json').read_text())
    if root.exists():
        shutil.rmtree(root)
    root.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cp', '-a', '--reflink=auto', str(previous), str(root)], check=True)
    subprocess.run(['chmod', '-R', 'u+w', str(root)], check=True)
    # --appimage-extract leaves directories 0700 and running the extracted tools leaves bytecode: neither is package content.
    for d in [root, *(x for x in root.rglob('*') if x.is_dir() and not x.is_symlink())]:
        if d.name == '__pycache__':
            shutil.rmtree(d, ignore_errors=True)
        elif d.exists():
            d.chmod(0o755)
    listed = {}
    for line in (previous / 'FILES.sha256').read_text().splitlines():
        digest, rel = line.split('  ', 1)
        listed[rel] = digest
    present = {x.relative_to(root).as_posix() for x in root.rglob('*') if x.is_file() and not x.is_symlink()} - {'FILES.sha256'}
    if present != set(listed):
        raise SystemExit('previous package is not pristine: ' + ', '.join(sorted(present ^ set(listed))[:8]))
    for rel, digest in listed.items():
        if sha(root / rel) != digest:
            raise SystemExit('previous package is not pristine: ' + rel)
    for src, dst in OVERLAY.items():
        mode = (root / dst).stat().st_mode if (root / dst).exists() else (repo / src).stat().st_mode
        (root / dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / src, root / dst)
        (root / dst).chmod(mode)
    if helper is not None:
        target = root / 'upscalers/helper/hlx_server.exe.so'
        if not target.is_file():
            raise SystemExit('no upscalers/helper/hlx_server.exe.so in the previous package')
        shutil.copyfile(helper, target)
        target.chmod(0o755)
        print(f'replaced upscalers/helper/hlx_server.exe.so: {sha(target)[:12]} from {helper}')
    elf_dir = root / 'emulator/elf'
    replaced = {}
    for name, elf in new_backends.items():
        target = elf_dir / name
        if target.is_symlink():
            raise SystemExit(name + ' is a link in the previous package; replace the backend it points to')
        if not target.is_file():
            raise SystemExit('no backend ' + name + ' in the previous package')
        aliases = [x.name for x in elf_dir.iterdir() if x.is_symlink() and x.readlink().name == name]
        shutil.copyfile(elf, target)
        target.chmod(0o755)
        for n in [name] + aliases:
            replaced[n] = {'sha256': sha(target), 'source': str(elf)}
    (root / 'VERSION').write_text(new_version + '\n')
    manifest = dict(old)
    manifest['version'] = new_version
    manifest['respin_of'] = old['version']
    manifest['changes_since_' + old['version'].rsplit('-', 1)[-1]] = CHANGES
    manifest['changed_files'] = sorted(OVERLAY.values()) + ['VERSION', 'BUILD-MANIFEST.json', 'VALIDATION.json', 'FILES.sha256'] + \
        sorted('emulator/elf/' + n for n in new_backends) + (['upscalers/helper/hlx_server.exe.so'] if helper else [])
    if replaced:
        head = subprocess.run(['git', '-C', str(repo), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
        manifest['emulator_commit'] = head
        manifest['backends'] = {**old['backends'], **{n: {'sha256': v['sha256'], 'commit': head} for n, v in replaced.items()}}
    (root / 'BUILD-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for name, info in manifest['backends'].items():
        if sha(root / 'emulator/elf' / name) != info['sha256']:
            raise SystemExit('backend changed: ' + name)
    for name, info in replaced.items():
        print(f'replaced {name}: {info["sha256"][:12]} from {info["source"]}')
    print('Staged ' + new_version + ' on ' + old['version'] + ': ' + str(root))


def finish(previous_appimage):
    validation = base / 'validation/VALIDATION.json'
    if not validation.is_file():
        raise SystemExit('write ' + str(validation) + ' first: the record of what was really tested')
    report = json.loads(validation.read_text())
    if report.get('release') != version():
        raise SystemExit('VALIDATION.json is for ' + str(report.get('release')))
    shutil.copyfile(validation, root / 'VALIDATION.json')
    for p in list(root.rglob('__pycache__')):
        if p.is_dir():
            shutil.rmtree(p)
    with (root / 'FILES.sha256').open('w') as f:
        for p in sorted(root.rglob('*')):
            if p.is_file() and not p.is_symlink() and p.name != 'FILES.sha256':
                f.write(sha(p) + '  ' + p.relative_to(root).as_posix() + '\n')
    dist.mkdir(exist_ok=True)
    name = f'KytyPS5-BC250-{version()}-x86_64'
    # The previous release's AppImage runtime (the part before its file system), so the tested runtime stays.
    previous_appimage = Path(previous_appimage)
    offset = int(subprocess.check_output([str(previous_appimage), '--appimage-offset'], text=True).strip())
    runtime = base / 'work/appimage-runtime'
    with previous_appimage.open('rb') as f:
        runtime.write_bytes(f.read(offset))
    tool = os.environ.get('APPIMAGETOOL') or shutil.which('appimagetool')
    if not tool:
        raise SystemExit('set APPIMAGETOOL')
    out = dist / (name + '.AppImage')
    with (base / 'validation/appimage-build.log').open('w') as log:
        subprocess.run([tool, '--runtime-file', str(runtime), '--comp', 'zstd', str(root), str(out)], check=True,
                       stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, ARCH='x86_64'))
    out.chmod(0o755)
    env = dict(os.environ, KYTY_BETA_BASE=str(base))
    for kind in ('tar', 'zip'):
        subprocess.run([sys.executable, str(Path(__file__).with_name('archive.py')), kind], check=True, env=env)
    for f in ('VALIDATION.json', 'BUILD-MANIFEST.json'):
        shutil.copyfile(root / f, dist / f)
    print('Built ' + name)


def minimal(previous_tarball):
    """Update the separate installer with the staged Minimal Mode session files."""
    new = f'KytyPS5-BC250-{version()}-PS5-Minimal-Mode'
    dist.mkdir(exist_ok=True)
    with tarfile.open(previous_tarball) as src, tarfile.open(dist / (new + '.tar.gz'), 'w:gz', compresslevel=6) as out:
        members = src.getmembers()
        top = members[0].name.split('/', 1)[0]
        session_updates = {p for p in OVERLAY.values() if p.startswith('system/session/')}
        for m in members:
            if m.name.removeprefix(top + '/') in session_updates:
                continue
            data = src.extractfile(m) if m.isfile() else None
            m.name = new + m.name[len(top):]
            if m.islnk():
                m.linkname = new + m.linkname[len(top):]
            out.addfile(m, data)
        for rel in sorted(session_updates):
            out.add(root / rel, arcname=f'{new}/{rel}')
    print('Repacked ' + new)


def source():
    name = f'KytyPS5-BC250-{version()}-source'
    dist.mkdir(exist_ok=True)
    if subprocess.run(['git', '-C', str(repo), 'status', '--porcelain'], capture_output=True, text=True).stdout.strip():
        raise SystemExit('commit the source tree first: the snapshot is of HEAD')
    subprocess.run(['git', '-C', str(repo), 'archive', '--format=tar.gz', '--prefix=' + name + '/', '-o',
                    str(dist / (name + '.tar.gz')), 'HEAD'], check=True)
    print('Source snapshot of ' + subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip())


def sums():
    v = version()
    with (dist / 'SHA256SUMS').open('w') as f:
        for p in sorted(dist.glob(f'KytyPS5-BC250-{v}-*')):
            f.write(sha(p) + '  ' + p.name + '\n')
    print((dist / 'SHA256SUMS').read_text(), end='')


if __name__ == '__main__':
    commands = {'stage': (stage, None), 'finish': (finish, 1), 'minimal': (minimal, 1), 'source': (source, 0), 'sums': (sums, 0)}
    if len(sys.argv) < 2 or sys.argv[1] not in commands or (
            len(sys.argv) - 2 != commands[sys.argv[1]][1] if commands[sys.argv[1]][1] is not None else len(sys.argv) < 4):
        raise SystemExit(__doc__)
    commands[sys.argv[1]][0](*sys.argv[2:])
