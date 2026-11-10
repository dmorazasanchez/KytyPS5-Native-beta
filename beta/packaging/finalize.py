#!/usr/bin/env python3
from pathlib import Path
import hashlib, json, shutil, subprocess
base = Path(__file__).resolve().parent.parent
root = base/'work/KytyPS5-Beta.AppDir'
dist = base/'dist'; dist.mkdir(exist_ok=True)
checks = json.loads((base/'validation/package-checks.json').read_text())
ui = json.loads((base/'validation/ui/result.json').read_text())
assert '113 checks passed' in (base/'validation/ubuntu-verify.log').read_text()
assert checks['passed'] == 39 and ui['passed'] == 111
assert (base/'validation/control-tests.log').read_text().count('Ran 10 tests') == 2
report = {
    'release':'2026.11.10-beta1',
    'launcher_checks':113,
    'headless_ui_checks':111,
    'package_routing_hash_checks':checks['passed'],
    'simulated_session_tests':20,
    'compatibility_environment':'Ubuntu 24.04 container; glibc 2.39; UID 12345; read-only /opt/kyty beta; no host game directories mounted',
    'wine_version':'wine-11.0 (CachyOS)',
    'host_doctor':'Passed on the development BC-250 CachyOS system',
    'gameplay_tested':False,
    'physical_bazzite_or_steamos_tested':False,
    'minimal_session_live_vt_handoff_tested':False,
    'limitations':['Helix generated networks and Lossless DLL require tester import','Standalone minimal mode requires host compositor/audio dependencies; non-Valve hosts return to their configured display-manager session/login','Latest VMID/recovery integration has not received a new gameplay retest']
}
(root/'VALIDATION.json').write_text(json.dumps(report,indent=2)+'\n')
manifest = json.loads((root/'BUILD-MANIFEST.json').read_text())
manifest['packaging_changes']=['Portable upstream .NET/Python and Debian SDL2/binutils/FFmpeg runtimes','Relocated current runtime selector and exact-SHA Astro binding','Beta-specific settings/saves and opt-in cheat plans','Account-specific helper/minimal session installer under /var and /etc','Launcher defaults to Upstream; writable exact-version cheat discovery; bundled FFmpeg path']
manifest['test_results']=report
(root/'BUILD-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
# No bytecode/build debris belongs in the read-only runtime package.
for p in list(root.rglob('__pycache__')):
    if p.is_dir(): shutil.rmtree(p)
with (root/'FILES.sha256').open('w') as f:
    for p in sorted(root.rglob('*')):
        if p.is_file() and not p.is_symlink() and p.name != 'FILES.sha256':
            digest=hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
            f.write(digest+'  '+p.relative_to(root).as_posix()+'\n')
shutil.copy2(root/'README.md',dist/'README-FIRST.md')
shutil.copy2(root/'VALIDATION.json',dist/'VALIDATION.json')
shutil.copy2(root/'BUILD-MANIFEST.json',dist/'BUILD-MANIFEST.json')
print('Release manifests finalized')
