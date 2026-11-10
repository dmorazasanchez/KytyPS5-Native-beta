#!/usr/bin/env python3
import ctypes, glob, json, os, platform, subprocess, sys
from pathlib import Path

def report():
    data = {'version': (Path(__file__).resolve().parent / 'VERSION').read_text().strip(),
            'os': Path('/etc/os-release').read_text() if Path('/etc/os-release').exists() else platform.platform(),
            'kernel': platform.release(), 'architecture': platform.machine(), 'checks': [], 'notes': []}
    def check(name, ok, detail): data['checks'].append({'name':name, 'ok': bool(ok), 'detail': detail})
    try:
        libc = ctypes.CDLL(None)
        libc.gnu_get_libc_version.restype = ctypes.c_char_p
        version = libc.gnu_get_libc_version().decode()
        check('glibc >= 2.38', tuple(map(int, version.split('.')[:2])) >= (2,38), version)
    except Exception as e: check('glibc >= 2.38', False, str(e))
    devices = []
    for p in Path('/sys/class/drm').glob('card[0-9]*/device'):
        try:
            vendor, device = (p / 'vendor').read_text().strip(), (p / 'device').read_text().strip()
            devices.append(vendor + ':' + device)
        except OSError: pass
    check('BC-250 GPU', any(x in ('0x1002:0x13fe', '0x1002:0x13f9') for x in devices), ', '.join(devices) or 'No GPU in this environment')
    nodes = glob.glob('/dev/dri/renderD*')
    check('DRM render access', any(os.access(p, os.R_OK|os.W_OK) for p in nodes), ', '.join(nodes) or 'No render nodes')
    check('KFD access', os.access('/dev/kfd', os.R_OK|os.W_OK), '/dev/kfd')
    check('GPU helper socket', Path('/run/kyty-lodhw.sock').is_socket(), '/run/kyty-lodhw.sock; install with --install-helper if missing')
    for name, path in [('GTT page limit','/sys/module/ttm/parameters/pages_limit'), ('AMDGPU scheduler','/sys/module/amdgpu/parameters/sched_policy'), ('VM update mode','/sys/module/amdgpu/parameters/vm_update_mode'), ('CWSR','/sys/module/amdgpu/parameters/cwsr_enable')]:
        try: data['notes'].append(name + ': ' + Path(path).read_text().strip())
        except OSError: pass
    for p in Path('/sys/class/drm').glob('card[0-9]*/device/mem_info_vram_total'):
        try: data['notes'].append('VRAM: ' + str(int(p.read_text()) // (1024*1024)) + ' MiB (512 MiB recommended for large games)')
        except OSError: pass
    data['notes'].append('Standalone minimal mode needs systemd >= 256, labwc, Xwayland, xdotool, PipeWire/Pulse, WirePlumber, PAM and polkit. Steam Game Mode can run the AppImage as a non-Steam application.')
    data['ok'] = all(c['ok'] for c in data['checks'])
    return data

if __name__ == '__main__':
    data = report()
    if '--json' in sys.argv: print(json.dumps(data, indent=2))
    else:
        print('KytyPS5 Beta ' + data['version'])
        for c in data['checks']: print(('PASS ' if c['ok'] else 'FAIL ') + c['name'] + ': ' + c['detail'])
        for note in data['notes']: print('INFO ' + note)
        print('No game launched and no system settings changed.')
    sys.exit(0 if data['ok'] else 1)
