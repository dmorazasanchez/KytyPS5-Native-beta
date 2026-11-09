"""Title-specific knowledge that cannot be derived from an interface: a game's own render-resolution controller.

An upscaler interface tells us where the inputs are; it does not say how the game picks its render resolution.
Fixed render presets therefore need an entry here, found once per executable (e.g. with a hardware write
watchpoint, NATIVE_UPSCALING_FINDINGS.md section 6). Without one, the upscaler still runs at the game's own
dynamic resolution. Entries are keyed by the exact executable SHA-256; the emulator also checks the table in memory.
"""

RESOLUTION_CONTROLLERS = {
    # Ghost of Yotei PPSA26344 02.002.000: found with a hardware write watchpoint on the render-size global
    # (kyty-native-upscaling-20261108 NATIVE_UPSCALING_FINDINGS.md section 6, run eLTUVq).
    '0c5377bb5fa74e80d57d4d9589430550721cac658ee4f259b837e371b2c99ca6': {
        'kind': 'level_table_override',
        'source': 'hardware write watchpoint on the render-size global (NATIVE_UPSCALING_FINDINGS.md section 6)',
        'table_rva': 0x1af8190,        # 16 (w, h) uint32 pairs, 3840x2160 down to 1920x1080 in 128x72 steps
        'levels': 16,
        'grid': [3840, 2160, 128, 72],
        'renderer_rva': 0x5093580,     # renderer singleton
        'override_offset': 0x32ee10,   # the title's fixed-level override (int32, -1 = dynamic)
        'index_offset': 0x32ee08,
        'table_pointer_offset': 0x32edc0,
        'render_size_rva': 0x53c20f0,
        'scale_pair_rva': 0x53c20b4,
        'output_requirement': [3840, 2160],
    },
}


def resolution_controller(sha256: str):
    """Known resolution controller for this exact executable, else None."""
    entry = RESOLUTION_CONTROLLERS.get(sha256)
    return dict(entry) if entry else None


# ---- TAA discoveries: a title's own TAA pass, as the discovery pipeline found it (pass capture, descriptor paths from
# the shader's instructions, temporal ping-pong, jitter, conventions from a probe). Keyed by executable SHA-256; shipped
# ones live next to this file (discovered/), the user's own under the profile store (discovered/).
import json as _json
import os as _os
from pathlib import Path as _Path


def _discovery_dirs():
    yield _Path(__file__).resolve().parent / 'discovered'
    env = _os.environ.get('KYTY_UPSCALER_PROFILE_DIR') or _os.environ.get('KYTYPS5_UPSCALER_PROFILE_DIR')
    root = _Path(env).expanduser() if env else _Path.home() / '.config' / 'KytyPS5-Native' / 'upscaler-profiles'
    yield root / 'discovered'


def taa_discovery(sha256: str):
    """The TAA discovery for this exact executable, else None."""
    for d in _discovery_dirs():
        p = d / f'{sha256}.json'
        try:
            data = _json.loads(p.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if data.get('sha256') == sha256 and data.get('adapter') == 'taa_paths_v1' and isinstance(data.get('taa'), dict):
            data['_file'] = str(p)
            return data
    return None
