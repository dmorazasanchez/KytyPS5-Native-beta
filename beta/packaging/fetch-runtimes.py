#!/usr/bin/env python3
from pathlib import Path
import hashlib, json, tarfile, urllib.request, lzma

base = Path(__file__).resolve().parent.parent
cache = base / 'downloads'
cache.mkdir(exist_ok=True)

def fetch(url, name, expected=None, algorithm='sha256'):
    target = cache / name
    if not target.exists():
        print('Downloading ' + name, flush=True)
        with urllib.request.urlopen(url, timeout=90) as source, target.open('wb') as dest:
            while chunk := source.read(1024 * 1024): dest.write(chunk)
    if expected:
        actual = hashlib.file_digest(target.open('rb'), algorithm).hexdigest()
        if actual != expected: raise RuntimeError('Checksum mismatch: ' + name)
    return target

release = json.loads((base / 'validation/dotnet-release.json').read_text())['releases'][0]
net = next(f for f in release['runtime']['files'] if f['name'] == 'dotnet-runtime-linux-x64.tar.gz')
archive = fetch(net['url'], 'dotnet-runtime.tar.gz', net['hash'], 'sha512')
(cache / 'dotnet').mkdir(exist_ok=True)
with tarfile.open(archive) as t: t.extractall(cache / 'dotnet', filter='data')
release = json.loads((base / 'validation/python-release.json').read_text())
py = next(a for a in release['assets'] if a['name'].startswith('cpython-3.14.') and a['name'].endswith('x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'))
archive = fetch(py['browser_download_url'], 'python.tar.gz', py['digest'].split(':')[1])
with tarfile.open(archive) as t: t.extractall(cache, filter='data')
# A real SDL2 build avoids pulling this rolling host's SDL3 compatibility library into the launcher.
index = fetch('https://deb.debian.org/debian/dists/bookworm/main/binary-amd64/Packages.xz', 'debian-Packages.xz')
for stanza in lzma.decompress(index.read_bytes()).decode().split('\n\n'):
    if stanza.startswith('Package: libsdl2-2.0-0\n'):
        info = dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line and not line.startswith(' '))
        deb = fetch('https://deb.debian.org/debian/' + info['Filename'], 'libsdl2.deb', info['SHA256'])
        import subprocess
        subprocess.run(['ar', 'x', str(deb)], cwd=cache, check=True)
        data = next(cache.glob('data.tar.*'))
        with tarfile.open(data) as t: t.extractall(cache / 'sdl2', filter='data')
        break
else: raise RuntimeError('SDL2 package not found')
binutils_packages = {}
stanzas = lzma.decompress(index.read_bytes()).decode().split('\n\n')
for name in ('binutils-x86-64-linux-gnu', 'libbinutils', 'libctf-nobfd0', 'libctf0'):
    stanza = next(s for s in stanzas if s.startswith('Package: ' + name + '\n'))
    meta = dict(line.split(': ', 1) for line in stanza.splitlines() if ': ' in line and not line.startswith(' '))
    deb = fetch('https://deb.debian.org/debian/' + meta['Filename'], name + '.deb', meta['SHA256'])
    import subprocess
    with subprocess.Popen(['ar', 'p', str(deb), 'data.tar.xz'], stdout=subprocess.PIPE) as ar:
        with tarfile.open(fileobj=ar.stdout, mode='r|xz') as t: t.extractall(cache / 'binutils', filter='data')
        if ar.wait(): raise RuntimeError('Failed to extract ' + name)
    binutils_packages[name] = meta
(cache / 'runtimes.json').write_text(json.dumps({'dotnet': net, 'python': {k:py[k] for k in ('name','browser_download_url','digest')}, 'sdl2': info, 'binutils': binutils_packages}, indent=2))
print('Runtime downloads verified', flush=True)
