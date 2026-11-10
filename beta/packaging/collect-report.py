#!/usr/bin/env python3
"""Create a local, bounded diagnostic artifact; never uploads anything."""
import datetime, importlib.util, json, os, re, subprocess, sys, tarfile, tempfile
from pathlib import Path
bundle = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('doctor', bundle / 'doctor.py')
doctor = importlib.util.module_from_spec(spec); spec.loader.exec_module(doctor)
output = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else Path.home() / ('KytyPS5-report-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.tar.gz')
def redact(s):
    s = s.replace(str(Path.home()), '<HOME>')
    return re.sub(r'(/[^\s\"]+\.zar)(?:!/[^\s\"]*)?', '<GAME-ARCHIVE>', s)
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'host.json').write_text(redact(json.dumps(doctor.report(), indent=2)))
    (tmp / 'BUILD-MANIFEST.json').write_bytes((bundle / 'BUILD-MANIFEST.json').read_bytes())
    for unit in ('kyty-lodhw', 'ps5-minimal-session', 'ps5-minimal-enter', 'ps5-minimal-return'):
        result = subprocess.run(['journalctl', '-b', '-u', unit, '-n', '150', '--no-pager'], capture_output=True, text=True, timeout=10)
        (tmp / (unit + '.log')).write_text(redact((result.stdout + result.stderr)[-150000:]))
    roots = [Path(os.environ['KYTYPS5_DATA_ROOT']) / 'logs', Path(os.environ['KYTYPS5_CONFIG_ROOT'])]
    logs = []
    for root in roots:
        if root.exists(): logs.extend(p for p in root.rglob('*') if p.is_file() and p.suffix in ('.log','.jsonl'))
    for i, f in enumerate(sorted(logs, key=lambda p:p.stat().st_mtime, reverse=True)[:8]):
        if f.stat().st_size > 8*1024*1024: continue
        (tmp / (str(i) + '-' + f.name)).write_text(redact(f.read_text(errors='replace')[-150000:]))
    with tarfile.open(output, 'w:gz') as t:
        for f in tmp.iterdir(): t.add(f, arcname=f.name)
print('Saved locally: ' + str(output))
print('Review the report before sharing. It contains system details and log excerpts; no saves or game files.')
