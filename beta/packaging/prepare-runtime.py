#!/usr/bin/env python3
"""Create only beta-owned writable state; rebind exact-SHA manual profiles on this host."""
import hashlib, json, os, sys
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
    try:
        with (profiles / 'global.json').open('x') as f:
            json.dump({'KYTY_NATIVE_GPU_PM4_MODE':'upstream', 'fg_mode':'off', 'framegen':'off', 'upscaler':'game'}, f, indent=2)
    except FileExistsError: pass
    for name in ('logs', 'upscalers/logs/ngx', 'upscalers/cache/hlx-game/vkd3d', 'upscalers/cache/hlx-game/mesa', 'astro'):
        (data / name).mkdir(parents=True, exist_ok=True)
    helper = data / 'upscalers/helper'
    if helper.is_symlink() and helper.resolve() != (bundle / 'upscalers/helper').resolve(): helper.unlink()
    if not helper.exists(): helper.symlink_to(bundle / 'upscalers/helper', target_is_directory=True)

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
    else: raise SystemExit('Usage: prepare-runtime.py --init | --astro GAME')
