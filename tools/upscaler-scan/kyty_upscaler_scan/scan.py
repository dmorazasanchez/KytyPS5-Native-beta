"""Build a game's upscaler profile: identification evidence, the selected route and what Kyty may enable for it.

Evidence ladder (per route):
  candidate               an interface is present (imports, names); not enough to hook
  static_partial          some structural checks failed; reported, never enabled
  static_layout_verified  hook and field identities established offline from the game's own code and data
  runtime_bindings_verified  (written by Kyty after play, in <profile>.runtime.json) the live bindings matched
Nothing here runs the game, touches the GPU, or writes game files.
"""
from __future__ import annotations

import datetime
import fcntl
import json
import os
from pathlib import Path

from . import ffx, game, knowledge, psml, selfelf, x86

SCANNER_VERSION = '1.1.0'
SCHEMA = 'kyty-upscaler-profile'
SCHEMA_VERSION = 2
MODULE_BASE = 0x900000000      # Kyty maps the game's main executable here (RuntimeLinker: SYSTEM_RESERVED+0x100000000)
TRINITY_ATTRIBUTE3_BIT = 1 << 22   # param.json attribute3: the title has a PS5 Pro (trinity) mode

# Runtime adapters this emulator build implements (src/graphics/native_gpu/nativeUpscaler.cpp).
IMPLEMENTED_ADAPTERS = {
    'ffx_agc_fsr3upscaler_v1': {'upscalers': ['hlx_fsr4', 'hlx_dlss_e'], 'frame_generation': ['fsr3']},
    'taa_paths_v1': {'upscalers': ['hlx_fsr4', 'hlx_dlss_e'], 'frame_generation': ['fsr3']},
}


def default_store() -> Path:
    # (KYTYPS5_*: survives the runtime selector's removal of KYTY_* overrides before it runs the scanner.)
    env = os.environ.get('KYTY_UPSCALER_PROFILE_DIR') or os.environ.get('KYTYPS5_UPSCALER_PROFILE_DIR')
    if env:
        return Path(env).expanduser()
    return Path.home() / '.config' / 'KytyPS5-Native' / 'upscaler-profiles'


def _now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def _hexify(o, keys_int=()):
    """Readable copy: addresses as hex strings (the `runtime` block keeps integers for the emulator)."""
    if isinstance(o, dict):
        return {k: (v if k in keys_int else _hexify(v)) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_hexify(v) for v in o]
    if isinstance(o, int) and not isinstance(o, bool) and (o > 0xffff or o < -0xff):
        return ('-' if o < 0 else '') + hex(abs(o))
    return o


def _route_id(r):
    return f"{r['family']}:{r.get('abi') or r.get('adapter') or r.get('component')}"


def _runtime_block(route, sha256, drs):
    rt = route['runtime']
    block = {
        'adapter': route['adapter'],
        'module_base': MODULE_BASE,
        'executor_return_rvas': rt['executor_return_rvas'],
        'executor_job_slot': rt['executor_job_slot'],
        'executor_backend_slot': rt['executor_backend_slot'],
        'dispatcher_range': rt['dispatcher_range'],
        'dispatcher_return_rva': rt.get('dispatcher_return_rva', 0),
        'dispatcher_description_slot': rt['dispatcher_description_slot'],
        'shader_header_range': rt['shader_header_range'],
        'shader_record_table': rt['shader_record_table'],
        'shader_record_first_header': rt['shader_record_first_header'],
        'header_magic': ffx.HEADER_MAGIC,
        # Checked in guest memory before anything is enabled: the running executable is the scanned one.
        'fingerprint': [
            {'rva': rt['shader_record_table'], 'u64_module_rva': rt['shader_record_first_header']},
            {'rva': rt['shader_record_first_header'], 'u32': ffx.HEADER_MAGIC},
        ],
        'resolution_controller': drs,
        'sha256': sha256,
    }
    return block


def build_profile(g: game.GameInput) -> dict:
    prof = {
        'schema': SCHEMA, 'schema_version': SCHEMA_VERSION, 'scanner_version': SCANNER_VERSION,
        'scanned_at': _now(), 'game_executed': False, 'gpu_used': False,
        'game': {
            'title_id': g.title_id, 'version': g.version, 'name': g.name, 'sha256': g.sha256,
            'executable_bytes': len(g.eboot), 'location': g.location, 'stamp': g.stamp,
            'attribute3': g.param.get('attribute3') if isinstance(g.param, dict) else None,
        },
    }
    a3 = prof['game']['attribute3']
    prof['game']['ps5_pro_enhanced'] = bool(isinstance(a3, int) and a3 & TRINITY_ATTRIBUTE3_BIT)
    try:
        img = selfelf.SelfImage(g.eboot)
    except selfelf.UnsupportedImage as e:
        prof['error'] = f'executable not analysable: {e}'
        prof['routes'] = []
        prof['capabilities'] = _capabilities(prof, None)
        return prof
    dis = x86.Disassembler(img)
    prof['imports'] = {'count': len(img.imports),
                       'libraries': sorted({i.library for i in img.imports.values()})}
    try:
        p = psml.analyse(img, dis)
        f = ffx.analyse(img, dis)
    except x86.DisassemblyUnavailable as e:
        prof['error'] = f'disassembler unavailable: {e}'
        prof['routes'] = []
        prof['capabilities'] = _capabilities(prof, None)
        return prof
    prof['psml'] = {k: v for k, v in p.items() if k != 'routes'}
    prof['fidelityfx'] = {k: v for k, v in f.items() if k != 'routes'}
    routes = p['routes'] + f['routes']
    # The title's own TAA, when the discovery pipeline has found it for this executable (base PS5 mode).
    disc = knowledge.taa_discovery(g.sha256)
    if disc is not None:
        verified = bool(disc.get('provenance', {}).get('runtime_bindings_verified'))
        routes.append({'family': 'taa', 'adapter': 'taa_paths_v1', 'mode_requirement': 'base',
                       'runtime_hook': 'translator_dispatch',
                       'state': 'runtime_bindings_verified' if verified else 'discovered',
                       'taa': disc['taa'], 'discovery': disc.get('_file'), 'provenance': disc.get('provenance', {}),
                       'checks': [{'check': 'TAA pass discovered for this executable', 'ok': True,
                                   'detail': disc.get('provenance', {}).get('method', '')}]})
    for r in routes:
        r['id'] = _route_id(r)
        impl = IMPLEMENTED_ADAPTERS.get(r.get('adapter') or '')
        r['emulator_support'] = 'implemented' if impl else 'not_implemented'
    prof['routes'] = routes
    drs = knowledge.resolution_controller(g.sha256)
    selected = None
    # Base-PS5 routes only (the PS5 Pro PSSR route cannot run on this hardware): FidelityFX first, then the title's TAA.
    for r in routes:
        if r['emulator_support'] == 'implemented' and r['state'] == 'static_layout_verified' and r['mode_requirement'] == 'base':
            selected = r
            break
    if selected is None:
        for r in routes:
            if r['emulator_support'] == 'implemented' and r.get('family') == 'taa' and r['mode_requirement'] == 'base':
                selected = r
                break
    if selected is not None and selected.get('family') == 'taa':
        prof['selected_route'] = selected['id']
        prof['runtime'] = {'adapter': 'taa_paths_v1', 'taa': selected['taa'], 'sha256': g.sha256}
    elif selected is not None:
        prof['selected_route'] = selected['id']
        prof['runtime'] = _runtime_block(selected, g.sha256, drs)
    else:
        prof['selected_route'] = None
        prof['runtime'] = None
    prof['capabilities'] = _capabilities(prof, selected, drs)
    return prof


def _capabilities(prof, selected, drs=None):
    routes = prof.get('routes', [])
    if selected is not None:
        impl = IMPLEMENTED_ADAPTERS[selected['adapter']]
        why_up = ('the game\'s own TAA pass was found for this executable; Kyty re-checks its live inputs before replacing it'
                  if selected.get('family') == 'taa' else
                  'interface found and verified in the executable; Kyty re-checks the live bindings before replacing the '
                  'game\'s upscaler')
        up = {'state': 'available', 'route': selected['id'], 'upscalers': impl['upscalers'], 'reason': why_up}
        fg = {'state': 'available', 'modes': impl['frame_generation'],
              'reason': 'depth and motion vectors come from the same verified boundary'}
        presets = ({'state': 'available', 'reason': 'render-resolution controller known for this executable'}
                   if drs else {'state': 'dynamic_only',
                                'reason': "the game's own dynamic resolution is kept (controller not mapped)"})
        return {'upscaler': up, 'frame_generation': fg, 'render_presets': presets}
    why = prof.get('error')
    pending = [r for r in routes if r['state'] in ('static_layout_verified', 'static_partial', 'candidate')]
    if why is None and pending:
        r = pending[0]
        if r['family'] == 'psml':
            why = (f"only a PS5 Pro (PSSR) route was found ({r['abi']}); its base-PS5 TAA has not been discovered yet "
                   f"(the TAA discovery run finds it)")
        elif r.get('adapter') and r['state'] != 'static_layout_verified':
            failed = [c['check'] for c in r.get('checks', []) if not c['ok']]
            why = f"FidelityFX {r.get('component')} found, adapter checks failed: {'; '.join(failed)}"
        else:
            why = f"FidelityFX {r.get('component')} found; no runtime adapter for it yet"
    elif why is None:
        why = 'no known upscaler interface (custom TAA/TSR or none); the game keeps its own image path'
    state = 'identified' if pending else 'unsupported'
    return {'upscaler': {'state': state, 'reason': why}, 'frame_generation': {'state': state, 'reason': why},
            'render_presets': {'state': 'unavailable', 'reason': why}}


def profile_name(prof):
    g = prof['game']
    title = ''.join(ch for ch in (g['title_id'] or 'UNKNOWN') if ch.isalnum())
    version = ''.join(ch for ch in (g['version'] or '0') if ch.isalnum() or ch == '.')
    return f"{title}-{version}-{g['sha256'][:12]}.json"


def _write_json(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f'.{path.name}.{os.getpid()}.tmp')
    tmp.write_text(json.dumps(data, indent=1, sort_keys=False) + '\n', encoding='utf-8')
    os.replace(tmp, path)


class Store:
    """Profiles by executable content; an index maps game paths to them so unchanged games are not re-read."""

    def __init__(self, root: Path | None = None):
        self.root = Path(root) if root else default_store()

    def _locked_index(self, update=None):
        self.root.mkdir(parents=True, exist_ok=True)
        lock = open(self.root / '.index.lock', 'a+')
        try:
            fcntl.flock(lock, fcntl.LOCK_EX)
            path = self.root / 'index.json'
            try:
                index = json.loads(path.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                index = {}
            if update is not None:
                update(index)
                _write_json(path, index)
            return index
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
            lock.close()

    def cached(self, path) -> Path | None:
        key = str(Path(path).expanduser().resolve())
        entry = self._locked_index().get(key)
        stamp = game.quick_stamp(path)
        if not entry or stamp is None or entry.get('stamp') != stamp or entry.get('scanner_version') != SCANNER_VERSION:
            return None
        p = self.root / entry['profile']
        return p if p.is_file() else None

    def scan(self, path, force=False) -> tuple[Path, dict, bool]:
        """(profile path, profile, freshly scanned)."""
        if not force:
            hit = self.cached(path)
            if hit is not None:
                return hit, json.loads(hit.read_text(encoding='utf-8')), False
        g = game.load(path)
        name = None
        # Same executable already profiled from another path (e.g. folder and archive): reuse it.
        for existing in self.root.glob(f'*-{g.sha256[:12]}.json') if self.root.is_dir() else []:
            try:
                old = json.loads(existing.read_text(encoding='utf-8'))
            except (OSError, ValueError):
                continue
            if old.get('game', {}).get('sha256') == g.sha256 and old.get('scanner_version') == SCANNER_VERSION and not force:
                name, prof = existing.name, old
                break
        if name is None:
            prof = build_profile(g)
            name = profile_name(prof)
            readable = _hexify({k: v for k, v in prof.items() if k != 'runtime'})
            readable['runtime'] = prof['runtime']
            _write_json(self.root / name, readable)
            prof = readable
        key = str(Path(path).expanduser().resolve())
        self._locked_index(lambda idx: idx.__setitem__(key, {'stamp': g.stamp, 'profile': name,
                                                              'sha256': g.sha256, 'scanner_version': SCANNER_VERSION,
                                                              'title_id': g.title_id, 'version': g.version}))
        return self.root / name, prof, True

    def runtime_results(self, profile_path: Path) -> dict | None:
        p = profile_path.with_suffix('.runtime.json')
        try:
            return json.loads(p.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return None


def status_line(prof, runtime=None) -> str:
    caps = prof.get('capabilities', {})
    up = caps.get('upscaler', {})
    if up.get('state') == 'available':
        verified = runtime and runtime.get('runtime_bindings_verified')
        tail = 'verified in play' if verified else 'Kyty checks the live bindings at the first frames'
        fg = caps.get('frame_generation', {}).get('state') == 'available'
        pre = caps.get('render_presets', {}).get('state') == 'available'
        return (f"HLX upscalers{' + frame generation' if fg else ''} available "
                f"({'presets' if pre else 'game resolution'}; {tail})")
    return ('Not available: ' if up.get('state') != 'identified' else 'Identified, not available: ') + up.get('reason', '')
