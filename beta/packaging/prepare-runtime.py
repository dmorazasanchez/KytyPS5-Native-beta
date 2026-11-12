#!/usr/bin/env python3
"""Create only beta-owned writable state; rebind exact-SHA manual profiles on this host."""
import hashlib, json, os, re, shutil, sys, zipfile
from pathlib import Path

bundle = Path(os.environ['KYTYPS5_BUNDLE_ROOT'])
data = Path(os.environ['KYTYPS5_DATA_ROOT'])

def initialize():
    root = data / 'runtime'
    for name in ('_SaveData', '_PipelineCache', '_TempData', '_DownloadData', '_Trophies', '_Patches'):
        (root / name).mkdir(parents=True, exist_ok=True)
    assets = root / 'assets'
    if assets.is_symlink() and assets.resolve() != (bundle / 'assets').resolve(): assets.unlink()
    if not assets.exists():
        try: assets.symlink_to(bundle / 'assets', target_is_directory=True)
        except FileExistsError: pass
    for f in (bundle / 'patches').glob('*.json'):
        try:
            with (root / '_Patches' / f.name).open('x') as target: target.write(f.read_text())
        except FileExistsError: pass
    config = Path(os.environ['KYTYPS5_CONFIG_ROOT'])
    profiles = Path(os.environ['KYTYPS5_PROFILE_ROOT'])
    profiles.mkdir(parents=True, exist_ok=True)
    # Exclusive creation preserves user changes and avoids two launchers racing first run.
    # Global defaults: the tested route. Per-title defaults ship in title-defaults/ (the launcher merges them).
    defaults = {'KYTY_NATIVE_GPU_PM4_MODE': 'upstream', 'fg_mode': 'off', 'framegen': 'auto', 'upscaler': 'auto',
                'upscale_preset': 'quality', 'resolution': '3840x2160', 'fullscreen': '1'}
    legacy = {'KYTY_NATIVE_GPU_PM4_MODE': 'upstream', 'fg_mode': 'off', 'framegen': 'off', 'upscaler': 'game'}
    target = profiles / 'global.json'
    try:
        with target.open('x') as f:
            json.dump(defaults, f, indent=2)
    except FileExistsError:
        # Betas 1-3 wrote `legacy` (Helix and frame generation off). Upgrade it once, only if never edited.
        try:
            if json.loads(target.read_text()) == legacy:
                target.write_text(json.dumps(defaults, indent=2))
        except (OSError, ValueError): pass
    for name in ('logs', 'upscalers/logs/ngx', 'upscalers/cache/hlx-game/vkd3d', 'upscalers/cache/hlx-game/mesa', 'astro'):
        (data / name).mkdir(parents=True, exist_ok=True)
    helper = data / 'upscalers/helper'
    if helper.is_symlink() and helper.resolve() != (bundle / 'upscalers/helper').resolve(): helper.unlink()
    if not helper.exists(): helper.symlink_to(bundle / 'upscalers/helper', target_is_directory=True)
    try: install_hlx_fsr4()
    except (OSError, zipfile.BadZipFile, KeyError) as error:
        # Never block a game over this: the selector falls back to the game's own upscaler.
        print(f'KytyPS5: could not prepare HLX FSR 4 ({error}); games use their own upscaler until it is fixed.', file=sys.stderr)


HELIX_ZIP = 'upscalers/HelixSR-3.2.0.zip'
HELIX_MEMBERS = {'HelixSR-3.2.0/HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll': 'dlls/hlx/amd_fidelityfx_upscaler_dx12.dll',
                 'HelixSR-3.2.0/HLX-FSR4/HLX-FSR4-built.txt': 'dlls/hlx/HLX-FSR4-built.txt',
                 'HelixSR-3.2.0/LICENSE': 'HelixSR-LICENSE', 'HelixSR-3.2.0/SOURCE.md': 'HelixSR-SOURCE.md',
                 'HelixSR-3.2.0/THIRD_PARTY_NOTICES.md': 'HelixSR-THIRD_PARTY_NOTICES.md'}


def install_hlx_fsr4():
    """HLX FSR 4 ships built in the official, unmodified HelixSR 3.2.0 release bundled with this package (its
    license allows redistributing the complete official release). Copy its DLL out once per release."""
    archive = bundle / HELIX_ZIP
    if not archive.is_file(): return
    up = data / 'upscalers'
    stamp = up / 'dlls/hlx/.from-bundled-release'
    tag = str(archive.stat().st_size)
    dll = up / 'dlls/hlx/amd_fidelityfx_upscaler_dx12.dll'
    if dll.is_file() and not stamp.is_file(): return          # imported by the user: keep theirs
    if dll.is_file() and stamp.read_text().strip() == tag: return
    with zipfile.ZipFile(archive) as z:
        for member, dest in HELIX_MEMBERS.items():
            target = up / dest
            target.parent.mkdir(parents=True, exist_ok=True)
            tmp = target.with_name(target.name + '.tmp')
            with z.open(member) as src, tmp.open('wb') as out: shutil.copyfileobj(src, out)
            tmp.replace(target)
    stamp.write_text(tag + '\n')


def steam_libraries():
    roots = [Path.home() / '.local/share/Steam', Path.home() / '.steam/steam', Path.home() / '.steam/root',
             Path.home() / '.var/app/com.valvesoftware.Steam/.local/share/Steam']
    seen = []
    for root in roots:
        vdf = root / 'steamapps/libraryfolders.vdf'
        candidates = [root]
        try: candidates += [Path(x) for x in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(errors='replace'))]
        except OSError: pass
        for c in candidates:
            try: r = c.resolve()
            except OSError: continue
            if r not in seen and (r / 'steamapps').is_dir(): seen.append(r)
    return seen


def lossless_dll():
    """The user's Lossless.dll: imported with --setup-lossless, or from a Steam install of Lossless Scaling."""
    imported = data / 'lossless/Lossless.dll'
    if imported.is_file(): return imported
    for lib in steam_libraries():
        f = lib / 'steamapps/common/Lossless Scaling/Lossless.dll'
        if f.is_file(): return f
    return None


def mako_config():
    """Write the MAKO Renderer profiles with the user's DLL; print the config path (nothing if no DLL)."""
    dll = lossless_dll()
    if dll is None: return
    path = Path(os.environ['KYTYPS5_CONFIG_ROOT']) / 'lsfg/mako.toml'
    path.parent.mkdir(parents=True, exist_ok=True)
    text = f"""# Written by KytyPS5 before each Lossless Scaling launch; edit the profiles in the launcher instead.
version = 2

[global]
dll = {json.dumps(str(dll))}
allow_fp16 = true

[[profile]]
name = "Kyty Fixed 2x"
frame_generation_provisioned = true
frame_generation_enabled = true
adaptive = false
multiplier = 2
flow_scale = 0.85
performance_mode = true
pacing = "none"

[[profile]]
name = "Kyty Adaptive 60 FPS"
frame_generation_provisioned = true
frame_generation_enabled = true
adaptive = true
target_fps = 60
adaptive_max_multiplier = 2
adaptive_stable_cadence = false
flow_scale = 0.85
performance_mode = true
pacing = "none"

[[profile]]
name = "Yotei Dynamic 60 FPS"
frame_generation_provisioned = true
frame_generation_enabled = true
adaptive = true
target_fps = 60
adaptive_target_refresh_rate = false
adaptive_max_multiplier = 2
adaptive_auto_base_fps_cap = false
adaptive_fractional_real_frame_priority = "auto"
adaptive_stable_cadence = false
base_fps_cap = 60
dynamic_cadence_recovery = false
gpu_headroom_percent = 0
scaling_enabled = false
flow_scale = 0.50
performance_mode = true
pacing = "none"
"""
    if not path.is_file() or path.read_text() != text: path.write_text(text)
    print(path)


def setup_lossless(source):
    src = Path(source).expanduser().resolve()
    if not src.is_file() or src.open('rb').read(2) != b'MZ': raise SystemExit(f'{src} is not a Windows DLL (Lossless.dll)')
    dest = data / 'lossless/Lossless.dll'
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    print('Imported your Lossless.dll for Lossless Scaling frame generation: ' + str(dest))

def astro(game_arg):
    sys.path.insert(0, str(bundle / 'tools/upscaler-scan'))
    from kyty_upscaler_scan import game
    path = game_arg.split('!/', 1)[0]
    expected = json.loads((bundle / 'upscalers/astro-templates/astro-manual.json').read_text())['game']
    # Metadata first, then a full executable digest. Never bind another region/version by title alone.
    key = hashlib.sha256(str(Path(path).resolve()).encode()).hexdigest()[:16]
    dest = data / 'astro' / key
    stamp = game.quick_stamp(path)
    base = dest / 'astro-manual.json'
    if base.exists():
        cached = json.loads(base.read_text())
        if cached['game'].get('stamp') == stamp:
            print(dest); return
    loaded = game.load(path)
    if loaded.title_id != expected['title_id'] or loaded.version != expected['version'] or loaded.sha256 != expected['sha256']:
        return
    dest.mkdir(parents=True, exist_ok=True)
    for f in (bundle / 'upscalers/astro-templates').glob('*.json'):
        obj = json.loads(f.read_text())
        if 'game' in obj:
            obj['game']['location'] = loaded.location
            obj['game']['stamp'] = loaded.stamp
        (dest / f.name).write_text(json.dumps(obj, indent=2) + '\n')
    print(dest)

def title(game_arg):
    path = Path(game_arg.split('!/', 1)[0])
    try:
        if path.suffix.lower() == '.zar' and path.is_file():
            sys.path.insert(0, str(bundle / 'tools/upscaler-scan'))
            from kyty_upscaler_scan.gamefs import Zar
            archive = Zar(path)
            try:
                candidates = sorted((p for p, is_file in archive.paths() if is_file and p.endswith('sce_sys/param.json')), key=lambda p:p.count('/'))
                if not candidates: return
                param = json.loads(archive.read_node(archive.lookup(candidates[0])[0]))
            finally: archive.close()
        else:
            root = path if path.is_dir() else path.parent
            param = json.loads((root / 'sce_sys/param.json').read_text())
        print(param.get('titleId', ''))
    except (OSError, ValueError, KeyError): return

if __name__ == '__main__':
    if sys.argv[1:] == ['--init']: initialize()
    elif len(sys.argv) == 3 and sys.argv[1] == '--astro': astro(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == '--title': title(sys.argv[2])
    elif sys.argv[1:] == ['--mako-config']: mako_config()
    elif sys.argv[1:] == ['--lossless']: print(lossless_dll() or '')
    elif len(sys.argv) == 3 and sys.argv[1] == '--setup-lossless': setup_lossless(sys.argv[2])
    else: raise SystemExit('Usage: prepare-runtime.py --init | --astro GAME | --title GAME | --mako-config | --lossless | --setup-lossless DLL')
