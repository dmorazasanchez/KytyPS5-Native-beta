"""Build shared native startup fix; preserve the current Astro objects verbatim otherwise.

No games, GPU probes, active binary replacements, or production build writes.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import shutil
import subprocess

audit = Path(__file__).resolve().parent
work = audit.parents[1]
prod = Path('/home/david/kyty-native-instructions-20261106')
build = prod / '_Build/native-instructions'
old_objects = Path('/home/david/kyty-auto-upscaler-20261109/experiments/auto-upscaler/build/objects')
parser = argparse.ArgumentParser()
parser.add_argument('--out-dir', type=Path, default=work / 'release')
parser.add_argument('--runtime-only', action='store_true')
args = parser.parse_args()
release = args.out_dir
release.mkdir(parents=True, exist_ok=True)
logs = audit / 'logs'
logs.mkdir(exist_ok=True)
objects = audit / 'main-objects'
objects.mkdir(exist_ok=True)
blocks = (build / 'build.ninja').read_text().split('\n\n')

def block(prefix):
    return next(x for x in blocks if x.startswith(prefix))

def fields(value):
    return dict(line.strip().split(' = ', 1) for line in value.splitlines()[1:] if ' = ' in line)

base = fields(block('build CMakeFiles/kyty_emulator.dir/src/libs/agc.cpp.o:'))
defines = shlex.split(base['DEFINES'])
flags = shlex.split(base['FLAGS'])
includes = [x.replace(str(prod) + '/src', str(work) + '/src') for x in shlex.split(base['INCLUDES'])]
link_block = block('build kyty_emulator:')
link_fields = fields(link_block)
original_inputs = shlex.split(link_block.splitlines()[0].split(': ', 1)[1])[1:]
for marker in ('|', '||'):
    if marker in original_inputs:
        original_inputs = original_inputs[:original_inputs.index(marker)]

sources = [
    'src/libs/agc.cpp', 'src/loader/runtimeLinker.cpp',
    'src/graphics/native_gpu/nativePm4.cpp', 'src/graphics/native_gpu/nativeShadow.cpp',
    'src/graphics/presentation/gameMenu.cpp', 'src/graphics/presentation/window/swapchain.cpp',
    'src/graphics/presentation/videoOut.cpp', 'src/graphics/native_gpu/nativeUpscaler.cpp',
    'src/graphics/presentation/window/frameGen.cpp', 'src/graphics/native_gpu/nativePassCapture.cpp',
]

def compile_one(item):
    source, output, experimental, overlay = item
    inc = includes[:]
    if overlay:
        inc.insert(0, '-I' + str(overlay / 'src'))
    if source.name == 'frameGen.cpp':
        inc.append('-I/home/david/kyty-auto-upscaler-20261109/3rdparty/fidelityfx/include')
    command = ['/usr/bin/clang++', *defines, *flags,
               *(['-DKYTY_ENABLE_EXPERIMENTAL_NATIVE_UPSCALER=1'] if experimental else []),
               *inc, '-c', str(source), '-o', str(output)]
    log_path = logs / (output.stem + '-build.log')
    # Resume an interrupted local build only when the exact command and all
    # local source/header timestamps still match its successful object.
    roots = [work / 'src'] + ([overlay / 'src'] if overlay else [])
    newest = max(p.stat().st_mtime_ns for root in roots for p in root.rglob('*')
                 if p.is_file() and p.suffix in ('.h', '.cpp'))
    if (output.is_file() and log_path.is_file() and output.stat().st_mtime_ns >= newest and
            log_path.read_text().splitlines()[0] == shlex.join(command)):
        print('reused', output.name, flush=True)
        return
    with log_path.open('w') as log:
        log.write(shlex.join(command) + '\n')
        log.flush()
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    print('compiled', output.name, flush=True)

jobs = [(work / src, objects / (Path(src).stem + '.o'), True, None) for src in sources]
jobs += [
    (work / 'src/graphics/native_gpu/nativeShadow.cpp', audit / 'nativeShadow-native.o', False, None),
    (audit / 'astro-source/src/graphics/native_gpu/nativeShadow.cpp', audit / 'nativeShadow-astro.o',
     True, audit / 'astro-source'),
]
stubs = audit / 'pass-capture-disabled.cpp'
shutil.copy2('/home/david/kyty-yotei-rt-fix-20261109/experiments/rt-fix/pass-capture-disabled.cpp', stubs)
jobs.append((stubs, audit / 'pass-capture-disabled.o', False, None))
jobs += [
    (work / 'src/loader/runtimeLinker.cpp', audit / 'runtimeLinker-native.o', False, None),
    (audit / 'astro-source/src/loader/runtimeLinker.cpp', audit / 'runtimeLinker-astro.o',
     True, audit / 'astro-source'),
]
if args.runtime_only:
    # Only runtimeLinker includes the new exception handler. Other source
    # objects must remain the exact previously validated inputs.
    previous = json.loads((audit / 'build-manifest.json').read_text())
    for src, expected in previous['main_sources'].items():
        if src != 'src/loader/runtimeLinker.cpp':
            assert hashlib.sha256((work / src).read_bytes()).hexdigest() == expected, src
    jobs = [job for job in jobs if job[0].name == 'runtimeLinker.cpp']

with ThreadPoolExecutor(max_workers=3) as pool:
    list(pool.map(compile_one, jobs))

def link(name, inputs):
    rsp = audit / (name + '.rsp')
    rsp.write_text('\n'.join(shlex.quote(x) for x in inputs) + '\n')
    linkflags = shlex.split(link_fields.get('LINK_FLAGS', ''))
    linkflags = [x.replace(str(build / 'kyty_emulator_clang_lld.map'), str(audit / (name + '.map')))
                 .replace('CMakeFiles/kyty_emulator.dir/link.d', str(audit / (name + '-link.d')))
                 for x in linkflags]
    target = release / name
    command = ['/usr/bin/clang++', *shlex.split(link_fields.get('FLAGS', '')), *linkflags,
               '@' + str(rsp), '-o', str(target)]
    with (logs / (name + '-link.log')).open('w') as log:
        log.write(shlex.join(command) + '\n')
        log.flush()
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    sha = hashlib.sha256(target.read_bytes()).hexdigest()
    print('linked', name, sha, flush=True)
    return sha

old_full_inputs = shlex.split((audit / 'upscaler-link.before.rsp').read_text())
snapshot_inputs = [x.replace(str(old_objects), str(audit / 'upscaler-objects')) for x in old_full_inputs]
baseline_sha = link('astro-relinked-baseline', snapshot_inputs)
expected_astro = hashlib.sha256(Path('/home/david/astro-helix-framegen-20261110/manual/kyty_emulator').read_bytes()).hexdigest()
assert baseline_sha == expected_astro, 'Astro baseline did not reproduce exactly; do not deploy.'

native_target = 'CMakeFiles/kyty_emulator.dir/src/graphics/native_gpu/nativeShadow.cpp.o'
assert native_target in original_inputs
native_replacements = {native_target: str(audit / 'nativeShadow-native.o'),
                       'CMakeFiles/kyty_emulator.dir/src/loader/runtimeLinker.cpp.o': str(audit / 'runtimeLinker-native.o')}
native_inputs = [native_replacements.get(x, x) for x in original_inputs]
native_inputs += [str(audit / 'pass-capture-disabled.o'), *shlex.split(link_fields.get('LINK_LIBRARIES', ''))]
native_sha = link('kyty_emulator_native', native_inputs)

main_inputs = []
for x in snapshot_inputs:
    p = Path(x)
    if p.parent == audit / 'upscaler-objects' and p.suffix == '.o':
        replacement = objects / p.name
        assert replacement.is_file(), replacement
        x = str(replacement)
    main_inputs.append(x)
main_sha = link('kyty_emulator_upscaling', main_inputs)

astro_old_shadow = str(audit / 'upscaler-objects/nativeShadow.o')
assert astro_old_shadow in snapshot_inputs
astro_replacements = {astro_old_shadow: str(audit / 'nativeShadow-astro.o'),
                      str(audit / 'upscaler-objects/runtimeLinker.o'): str(audit / 'runtimeLinker-astro.o')}
astro_inputs = [astro_replacements.get(x, x) for x in snapshot_inputs]
astro_sha = link('kyty_emulator_astro', astro_inputs)

# Distinct paths preserve the selector's existing backend-specific branches.
for name in ('kyty_emulator_auto_upscaler', 'kyty_emulator_launcher_taa_fg', 'kyty_emulator_native_upscaler'):
    shutil.copy2(release / 'kyty_emulator_upscaling', release / name)

manifest = {
    'native_sha256': native_sha, 'upscaling_sha256': main_sha, 'astro_sha256': astro_sha,
    'astro_baseline_reproduced_sha256': baseline_sha,
    'astro_change': 'Only nativeShadow.o and runtimeLinker.o replaced; remaining Astro objects preserved byte for byte.',
    'main_sources': {src: hashlib.sha256((work / src).read_bytes()).hexdigest() for src in sources},
    'production_native_sha256_unchanged': hashlib.sha256((prod / 'release/kyty_emulator').read_bytes()).hexdigest(),
    'default_native_reservation': True, 'requires_test_env': False, 'title_specific_enable_check': False,
    'game_launched': False,
}
(audit / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2), flush=True)
