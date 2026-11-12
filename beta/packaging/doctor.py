#!/usr/bin/env python3
import ctypes, glob, json, os, platform, subprocess, sys
from pathlib import Path

def report():
    data = {'version': (Path(__file__).resolve().parent / 'VERSION').read_text().strip(),
            'os': Path('/etc/os-release').read_text() if Path('/etc/os-release').exists() else platform.platform(),
            'kernel': platform.release(), 'architecture': platform.machine(), 'checks': [], 'notes': [], 'warnings': []}
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
    check('GPU helper socket', Path('/run/kyty-lodhw.sock').is_socket(), '/run/kyty-lodhw.sock; run --prepare-system if missing')
    for name, path in [('GTT page limit','/sys/module/ttm/parameters/pages_limit'), ('AMDGPU scheduler','/sys/module/amdgpu/parameters/sched_policy'), ('VM update mode','/sys/module/amdgpu/parameters/vm_update_mode'), ('CWSR','/sys/module/amdgpu/parameters/cwsr_enable')]:
        try: data['notes'].append(name + ': ' + Path(path).read_text().strip())
        except OSError: pass
    if any(device in ('0x1002:0x13fe', '0x1002:0x13da') for device in devices):
        # Match nativeDefaults.cpp: patched AMDGPU supplies its VM/wave-save setup.
        patched = Path('/sys/module/amdgpu/parameters/bc250_gfx_queues').exists()
        expected = {'sched_policy': '2'} if patched else {
            'sched_policy': '2', 'vm_update_mode': '0', 'cwsr_enable': '0'}
        observed = {}
        for name in expected:
            try: observed[name] = Path('/sys/module/amdgpu/parameters', name).read_text().strip()
            except OSError: observed[name] = 'unavailable'
        data['native_gpu_options_recommended'] = all(observed[name] == value for name, value in expected.items())
        if not data['native_gpu_options_recommended']:
            mismatch = ', '.join(f'{name}={observed[name]} (recommended {value})'
                                 for name, value in expected.items() if observed[name] != value)
            data['warnings'].append('Native GPU driver options differ: ' + mismatch +
                '. Kyty disables its default KFD queue path unless sched_policy=2: games can stall on a black screen '
                '(Ghost of Yotei refuses COND_EXEC). Fix: run --prepare-system, then reboot.')
        try: gtt_pages = int(Path('/sys/module/ttm/parameters/pages_limit').read_text().strip())
        except (OSError, ValueError): gtt_pages = 0
        if gtt_pages and gtt_pages < 3670016:
            data['warnings'].append(f'GTT limit is {gtt_pages * 4 // 1024} MiB (ttm.pages_limit={gtt_pages}); large games need 14 GiB '
                '(ttm.pages_limit=3670016) or stall while loading. Fix: run --prepare-system, then reboot.')
        data['notes'].append('AMDGPU: ' + ('patched BC-250 module' if patched else 'stock module'))
        if not patched:
            data['warnings'].append('Stock amdgpu: games run, but heavy titles leave less memory for frame generation. '
                'Measured with Ghost of Yotei, HLX FSR 4 Quality and FSR 3 frame generation: about 400 MiB more GPU memory '
                'than with the patched module, so frame generation stayed paused waiting for memory. For frame generation, '
                'use the patched amdgpu (README: Optional patched amdgpu).')
        governors = ('cyan-skillfish-governor-smu', 'cyan-skillfish-governor', 'cyan-skillfish-governor-tt', 'oberon-governor')
        try:
            running = [g for g in governors if subprocess.run(['systemctl', 'is-active', '--quiet', g + '.service'],
                       timeout=5).returncode == 0]
        except (OSError, subprocess.SubprocessError): running = None
        if running == []:
            data['warnings'].append('No BC-250 GPU clock governor is running, so the GPU may stay near its 1500 MHz default. '
                'Upscalers cost more time per frame (HLX FSR 4 measured 5.5 ms instead of about 4.2 ms). Install and enable a '
                'BC-250 governor (for example cyan-skillfish-governor) with a range suited to your cooling.')
        elif running:
            data['notes'].append('GPU clock governor: ' + ', '.join(running))
    for p in Path('/sys/class/drm').glob('card[0-9]*/device/mem_info_vram_total'):
        try: data['notes'].append('VRAM: ' + str(int(p.read_text()) // (1024*1024)) + ' MiB (512 MiB recommended for large games)')
        except OSError: pass
    # Upscalers and frame generation readiness (the runtime prepares HLX FSR 4 from the bundled HelixSR release).
    bundle = Path(__file__).resolve().parent
    data_root = Path(os.environ.get('KYTYPS5_DATA_ROOT', str(Path.home() / '.local/share/KytyPS5-Beta')))
    up = data_root / 'upscalers/dlls'
    fsr4 = (up / 'hlx/amd_fidelityfx_upscaler_dx12.dll').is_file() or (bundle / 'upscalers/HelixSR-3.2.0.zip').is_file()
    data['notes'].append('HLX FSR 4: ' + ('ready' if fsr4 else 'missing (package incomplete)'))
    data['notes'].append('HLX DLSS: ' + ('ready' if (up / 'hlx-dlss/helixsr.dll').is_file() else 'not built (optional: --setup-helix-dlss)'))
    try:
        lossless = subprocess.run([sys.executable, str(bundle / 'prepare-runtime.py'), '--lossless'], text=True, capture_output=True,
                                  timeout=20, env=dict(os.environ, KYTYPS5_BUNDLE_ROOT=str(bundle), KYTYPS5_DATA_ROOT=str(data_root))).stdout.strip()
    except (OSError, subprocess.SubprocessError): lossless = ''
    data['notes'].append('Lossless Scaling frame generation: ' + ('ready (' + lossless + ')' if lossless else
                         'needs your Lossless.dll (install Lossless Scaling from Steam, or --setup-lossless /path/to/Lossless.dll)'))
    data['notes'].append('PS5 Minimal Mode: ' + ('installed' if Path('/var/lib/kyty-beta-system/session/ps5-minimal-control').is_file() else 'not installed (optional: --install-gaming-mode)'))
    data['ok'] = all(c['ok'] for c in data['checks'])
    return data

if __name__ == '__main__':
    data = report()
    if '--json' in sys.argv: print(json.dumps(data, indent=2))
    else:
        print('KytyPS5 Beta ' + data['version'])
        for c in data['checks']: print(('PASS ' if c['ok'] else 'FAIL ') + c['name'] + ': ' + c['detail'])
        for warning in data['warnings']: print('WARN ' + warning)
        for note in data['notes']: print('INFO ' + note)
        print('No game launched and no system settings changed.')
    sys.exit(0 if data['ok'] else 1)
