#!/usr/bin/env python3
from pathlib import Path
import hashlib, json, os, subprocess, tempfile
base = Path(__file__).resolve().parent.parent
bundle = base / 'work/KytyPS5-Beta.AppDir'
checks = []
def check(ok, name):
    if not ok: raise AssertionError(name)
    checks.append(name)
    print('PASS ' + name, flush=True)

manifest = json.loads((bundle / 'BUILD-MANIFEST.json').read_text())
for name, info in manifest['backends'].items():
    check(hashlib.file_digest((bundle / 'emulator/elf' / name).open('rb'),'sha256').hexdigest() == info['sha256'], 'Unchanged backend: ' + name)
for f in bundle.rglob('*'):
    if f.is_symlink():
        check(not os.path.isabs(os.readlink(f)) and f.exists(), 'Relocatable symlink: ' + str(f.relative_to(bundle))) if os.path.isabs(os.readlink(f)) or not f.exists() else None
check(all(not os.path.isabs(os.readlink(f)) and f.exists() for f in bundle.rglob('*') if f.is_symlink()), 'All runtime symlinks are relative and resolve')
for f in (bundle / 'patches').glob('*.json'):
    p = json.loads(f.read_text())
    check(all(not m.get('enabled') for m in p['mods']), 'Cheats initially disabled: ' + f.name)
for f in [bundle/'AppRun', bundle/'runtime-selector.sh', bundle/'selector-relocated.sh']:
    check('/home/david' not in f.read_text(), 'No developer path in ' + f.name)
    subprocess.run(['bash','-n',str(f)],check=True)
with tempfile.TemporaryDirectory(prefix='kyty-beta-package-') as temp:
    temp = Path(temp)
    env = os.environ.copy()
    env.update(KYTYPS5_BUNDLE_ROOT=str(bundle),KYTYPS5_DATA_ROOT=str(temp/'data'),KYTYPS5_CONFIG_ROOT=str(temp/'config'),KYTYPS5_PROFILE_ROOT=str(temp/'profiles'),PATH=str(bundle/'bin')+':'+env['PATH'])
    subprocess.run([str(bundle/'bin/python3'),str(bundle/'prepare-runtime.py'),'--init'],env=env,check=True)
    profile = temp/'profile.json'
    scanner = temp/'scanner'
    scanner.write_text('import sys\nprint(' + repr(str(profile)) + ')\n')
    env.update(KYTYPS5_UPSCALER_SCANNER=str(scanner),KYTY_EMULATOR_SELECTOR_DRY_RUN='1',KYTY_NATIVE_GPU_PM4_MODE='upstream')
    def route(extra, name, binary, contains=(), absent=(), args=None):
        e = env.copy(); e.update(extra)
        r = subprocess.run([str(bundle/'selector-relocated.sh'),*(args or ['--game',str(temp/'missing/eboot.bin')])],env=e,text=True,capture_output=True)
        check(r.returncode == 0, name + ' exits successfully: ' + r.stderr[-200:] if r.returncode else name + ' exits successfully')
        check('Executable: '+str(bundle/'emulator'/binary) in r.stdout, name + ' selects ' + binary)
        check(all(s in r.stdout for s in contains) and all(s not in r.stdout for s in absent), name + ' settings propagate')
    route({}, 'Native', 'kyty_emulator_native')
    route({'KYTY_NATIVE_GPU_PM4_MODE':'fast'}, 'Legacy Fast', 'kyty_emulator_fast')
    for adapter,binary in [('fsr3_description_v1','kyty_emulator_auto_upscaler'),('taa_paths_v1','kyty_emulator_launcher_taa_fg')]:
        profile.write_text(json.dumps({'capabilities':{'upscaler':{'state':'available'},'frame_generation':{'state':'available'}},'runtime':{'adapter':adapter}}))
        extra={'KYTY_NATIVE_UPSCALER_BUILD':'1','KYTY_UPSCALER':'game','KYTY_FRAMEGEN':'fsr3'}
        route(extra, adapter, binary, ['KYTY_FRAMEGEN=fsr3'])
    route(dict(extra,KYTY_BC250_TITLE_ID='PPSA01341',KYTY_DEMONS_SCREAM_ASSERT_RECOVERY='1'), 'Recovery after TAA selection', 'kyty_emulator_launcher_taa_fg', ['KYTY_DEMONS_SCREAM_ASSERT_RECOVERY=1'])
    route(dict(extra,KYTY_BC250_TITLE_ID='PPSA01341',KYTY_DEMONS_SCREAM_ASSERT_RECOVERY='0'), 'Recovery Off', 'kyty_emulator_launcher_taa_fg', absent=['KYTY_DEMONS_SCREAM_ASSERT_RECOVERY=1'])
    # Read-only validation against the developer's current real archive; never execute the game.
    original = Path('/home/david/astro-helix-framegen-20261110/manual/astro-manual.json')
    g = json.loads(original.read_text())['game']['location']
    archive = g['archive'] + '!/' + g['member'].rsplit('/',1)[0]
    route({'KYTY_NATIVE_UPSCALER_BUILD':'1','KYTY_UPSCALER':'game','KYTY_FRAMEGEN':'fsr3'}, 'Astro exact-SHA relocation', 'kyty_emulator_astro', ['KYTY_ASTRO_DECOUPLE=1'], args=['--game',archive])
    # Repeated package mounts must refresh only beta-owned asset/helper links.
    assetlink = temp/'data/runtime/assets'
    assetlink.unlink(); assetlink.symlink_to('/tmp/kyty-previous-mount/assets')
    subprocess.run([str(bundle/'bin/python3'),str(bundle/'prepare-runtime.py'),'--init'],env=env,check=True)
    check(assetlink.resolve() == (bundle/'assets').resolve(),'Asset links refresh after AppImage remount/update')
check(not (bundle/'upscalers/dlls').exists(), 'No generated Helix network/shader DLLs bundled')
(base/'validation/package-checks.json').write_text(json.dumps({'passed':len(checks),'checks':checks,'game_executed':False},indent=2)+'\n')
print(str(len(checks)) + ' package checks passed')
