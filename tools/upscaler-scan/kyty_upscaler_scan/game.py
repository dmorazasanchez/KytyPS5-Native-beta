"""Locate a game's executable and metadata in an extracted folder or a .zar archive (read only)."""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path

from . import gamefs


@dataclass
class GameInput:
    path: str                      # what the user/launcher gave us
    eboot: bytes
    param: dict
    location: dict                 # {'file': ...} or {'archive': ..., 'member': ...}
    stamp: str                     # cheap change detector (size/mtime of the file or archive)
    sha256: str = field(init=False)

    def __post_init__(self):
        self.sha256 = hashlib.sha256(self.eboot).hexdigest()

    @property
    def title_id(self):
        return self.param.get('titleId', '') if isinstance(self.param, dict) else ''

    @property
    def version(self):
        return self.param.get('contentVersion', '') if isinstance(self.param, dict) else ''

    @property
    def name(self):
        p = self.param if isinstance(self.param, dict) else {}
        loc = p.get('localizedParameters') or {}
        for lang in ('en-US', p.get('defaultLanguage', 'en-US')):
            n = (loc.get(lang) or {}).get('titleName')
            if n:
                return n
        return self.title_id


def _stamp(path: Path) -> str:
    st = path.stat()
    return f'{st.st_size}|{st.st_mtime_ns}'


def quick_stamp(path) -> str | None:
    """The change detector for `path` without reading the executable (None: not a game path)."""
    path = Path(path).expanduser().resolve()
    if path.suffix.lower() == '.zar' and path.is_file():
        return _stamp(path)
    eboot = path / 'eboot.bin' if path.is_dir() else path
    if eboot.is_file():
        return _stamp(eboot)
    return None


def load(path) -> GameInput:
    raw_path = str(path)
    path = Path(path).expanduser().resolve()
    if path.suffix.lower() == '.zar' and path.is_file():
        archive = gamefs.Zar(path)
        try:
            members = [p for p, is_file in archive.paths() if is_file and p.rsplit('/', 1)[-1].lower() == 'eboot.bin']
            # The game root: the shallowest eboot.bin with sce_sys/param.json beside it (same rule as the launcher).
            members.sort(key=lambda p: p.count('/'))
            member = next((m for m in members if archive.lookup((m.rsplit('/', 1)[0] + '/' if '/' in m else '') +
                                                                 'sce_sys/param.json')), None)
            if member is None:
                raise FileNotFoundError(f'{path}: no eboot.bin with sce_sys/param.json in the archive')
            prefix = member.rsplit('/', 1)[0] + '/' if '/' in member else ''
            eboot = archive.read_node(archive.lookup(member)[0])
            param = json.loads(archive.read_node(archive.lookup(prefix + 'sce_sys/param.json')[0]))
            return GameInput(raw_path, eboot, param, {'archive': str(path), 'member': member}, _stamp(path))
        finally:
            archive.close()
    eboot_path = path / 'eboot.bin' if path.is_dir() else path
    if not eboot_path.is_file():
        raise FileNotFoundError(f'{path}: no eboot.bin')
    param_path = eboot_path.parent / 'sce_sys' / 'param.json'
    param = json.loads(param_path.read_text(encoding='utf-8')) if param_path.is_file() else {}
    return GameInput(raw_path, eboot_path.read_bytes(), param, {'file': str(eboot_path)}, _stamp(eboot_path))
