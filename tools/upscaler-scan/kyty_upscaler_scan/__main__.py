"""kyty-upscaler-scan: identify a game's upscaler interface offline and record what Kyty may enable for it.

  scan <game>...        scan (cached by path + size/mtime; --force re-reads), print a summary
  ensure <game>         scan if needed; print the profile path only (for the runtime selector)
  status <game>         one JSON line {state, profile, line} for the launcher (scans if needed)
  library [config]      scan every game folder in the launcher's config (default ~/.config/KytyPS5/launcher.json)
  show <game>           print the full profile JSON

<game> is a game folder, its eboot.bin, or a .zar archive. Profiles go to ~/.config/KytyPS5-Native/upscaler-profiles
(override: --store DIR or KYTY_UPSCALER_PROFILE_DIR). The game is never executed and no game file is written.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from . import scan as S


def _summary(path, prof, fresh):
    caps = prof.get('capabilities', {})
    g = prof.get('game', {})
    lines = [f"{g.get('name') or g.get('title_id')} [{g.get('title_id')} {g.get('version')}] "
             f"{'scanned' if fresh else 'cached'}: {path}"]
    for r in prof.get('routes', []):
        failed = [c['check'] for c in r.get('checks', []) if not c.get('ok')]
        lines.append(f"  route {r['id']}: {r['state']}, emulator {r['emulator_support']}, mode {r['mode_requirement']}"
                     + (f"; failed: {'; '.join(failed)}" if failed else ''))
    for k in ('upscaler', 'frame_generation', 'render_presets'):
        c = caps.get(k, {})
        lines.append(f"  {k}: {c.get('state')} - {c.get('reason')}")
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='kyty-upscaler-scan', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--store', type=Path, default=None)
    ap.add_argument('--force', action='store_true')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('scan', 'show'):
        p = sub.add_parser(name)
        p.add_argument('games', nargs='+')
    for name in ('ensure', 'status'):
        p = sub.add_parser(name)
        p.add_argument('game')
    p = sub.add_parser('library')
    p.add_argument('config', nargs='?', default=str(Path.home() / '.config/KytyPS5/launcher.json'))
    a = ap.parse_args(argv)
    store = S.Store(a.store)
    rc = 0
    if a.cmd in ('scan', 'show'):
        for gpath in a.games:
            try:
                path, prof, fresh = store.scan(gpath, force=a.force)
            except (OSError, ValueError) as e:
                print(f'{gpath}: {e}', file=sys.stderr)
                rc = 1
                continue
            print(json.dumps(prof, indent=1) if a.cmd == 'show' else _summary(path, prof, fresh))
    elif a.cmd == 'ensure':
        try:
            path, _prof, _fresh = store.scan(a.game, force=a.force)
        except (OSError, ValueError) as e:
            print(f'{a.game}: {e}', file=sys.stderr)
            return 1
        print(path)
    elif a.cmd == 'status':
        try:
            path, prof, _fresh = store.scan(a.game, force=a.force)
            rt = store.runtime_results(path)
            up = prof.get('capabilities', {}).get('upscaler', {})
            print(json.dumps({'state': up.get('state'), 'profile': str(path), 'line': S.status_line(prof, rt),
                              'title_id': prof['game'].get('title_id'), 'version': prof['game'].get('version'),
                              'runtime_verified': bool(rt and rt.get('runtime_bindings_verified'))}))
        except (OSError, ValueError) as e:
            print(json.dumps({'state': 'error', 'line': f'Scan failed: {e}'}))
            return 1
    elif a.cmd == 'library':
        cfg = json.loads(Path(a.config).expanduser().read_text(encoding='utf-8'))
        seen = set()
        for root in cfg.get('GameFolders', []):
            for cand in _games_under(Path(root)):
                if cand in seen:
                    continue
                seen.add(cand)
                try:
                    path, prof, fresh = store.scan(cand, force=a.force)
                    print(_summary(path, prof, fresh))
                except (OSError, ValueError) as e:
                    print(f'{cand}: {e}', file=sys.stderr)
                    rc = 1
    return rc


def _games_under(root: Path, depth=0):
    """Same discovery rule as the launcher's library scan: eboot.bin folders and .zar files, 4 levels deep."""
    out = []
    if root.is_file():
        return [str(root)] if root.suffix.lower() == '.zar' or root.name.lower() == 'eboot.bin' else []
    if not root.is_dir():
        return out
    if (root / 'eboot.bin').is_file():
        return [str(root)]
    try:
        entries = sorted(root.iterdir())
    except OSError:
        return out
    for e in entries:
        if e.is_file() and e.suffix.lower() == '.zar':
            out.append(str(e))
    if depth < 4:
        for e in entries:
            if e.is_dir() and not e.is_symlink():
                out += _games_under(e, depth + 1)
    return out


if __name__ == '__main__':
    sys.exit(main())
