#!/usr/bin/env python3
"""Targeted experimental build: compiles only the upscaler sources of this worktree with the
production build's exact flags, links them with the production build's other objects (read only),
and writes a NEW executable here. Never runs ninja in, or writes to, the production tree."""
import hashlib, json, shlex, subprocess, sys
from pathlib import Path
here = Path(__file__).resolve().parent
work = here.parents[2]
prod = Path('/home/david/kyty-native-instructions-20261106')
build = prod / '_Build/native-instructions'
out = here / 'objects'; out.mkdir(exist_ok=True)
logs = here / 'logs'; logs.mkdir(exist_ok=True)
sources = ['src/libs/agc.cpp', 'src/loader/runtimeLinker.cpp', 'src/graphics/native_gpu/nativePm4.cpp', 'src/graphics/native_gpu/nativeShadow.cpp',
           'src/graphics/presentation/gameMenu.cpp', 'src/graphics/presentation/window/swapchain.cpp',
           'src/graphics/presentation/videoOut.cpp']
new_sources = ['src/graphics/native_gpu/nativeUpscaler.cpp', 'src/graphics/presentation/window/frameGen.cpp',
               'src/graphics/native_gpu/nativePassCapture.cpp']
# AMD FidelityFX SDK 1.1.4 (MIT): frame interpolation + optical flow, Vulkan backend, with Kyty's BC-250 shader changes.
ffx = work / '3rdparty/fidelityfx'
ffx_sources = ['src/backends/vk/ffx_vk.cpp', 'src/backends/vk/kyty_swapchain_stub.cpp', 'src/backends/shared/ffx_shader_blobs.cpp',
               'src/components/frameinterpolation/ffx_frameinterpolation.cpp', 'src/components/opticalflow/ffx_opticalflow.cpp',
               'src/shared/ffx_assert.cpp', 'src/shared/ffx_breadcrumbs_list.cpp', 'src/shared/ffx_message.cpp',
               'src/shared/ffx_object_management.cpp']
text = (build / 'build.ninja').read_text()
blocks = text.split('\n\n')
def block(prefix):
    return next(b for b in blocks if b.startswith(prefix))
def fields(b):
    f = {}
    for line in b.splitlines()[1:]:
        if ' = ' in line:
            k, v = line.strip().split(' = ', 1); f[k] = v
    return f
base = fields(block('build CMakeFiles/kyty_emulator.dir/src/libs/agc.cpp.o:'))
def remap(s):
    return s.replace(str(prod) + '/src', str(work) + '/src').replace(str(prod) + '/3rdparty', str(prod) + '/3rdparty')
defines = shlex.split(base['DEFINES']) + ['-DKYTY_ENABLE_EXPERIMENTAL_NATIVE_UPSCALER=1']
includes = [remap(x) for x in shlex.split(base['INCLUDES'])] + ['-I' + str(ffx / 'include')]
flags = shlex.split(base['FLAGS'])
compiler = block('rule CXX_COMPILER__kyty_emulator_unscanned_Release').split('command = ')[1].split()[0] if 'rule CXX_COMPILER__kyty_emulator_unscanned_Release' in text else '/usr/bin/clang++'
only = [a for a in sys.argv[1:] if not a.startswith('--')]
objects = {}
for src in sources + new_sources:
    obj = out / (Path(src).stem + '.o')
    objects[src] = obj
    if only and Path(src).stem not in only and obj.exists():
        continue
    cmd = [compiler, *defines, *flags, *includes, '-c', str(work / src), '-o', str(obj)]
    with (logs / (Path(src).stem + '-build.log')).open('w') as log:
        log.write(shlex.join(cmd) + '\n')
        r = subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode:
        print((logs / (Path(src).stem + '-build.log')).read_text()[-4000:])
        raise SystemExit(f'compile failed: {src}')
    print('compiled', src)
# FidelityFX: shader blobs (glslangValidator) when any shader or the generator changed, then its sources.
ffx_out = out / 'fidelityfx'; ffx_out.mkdir(exist_ok=True)
blobs = ffx_out / 'ffx_shaderblobs_vk.cpp'
shader_files = [f for d in ('include/FidelityFX/gpu', 'src/backends/vk/shaders') for f in (ffx / d).rglob('*') if f.is_file()]
if not blobs.exists() or any(f.stat().st_mtime > blobs.stat().st_mtime for f in shader_files + [ffx / 'gen_shaderblobs.py']):
    r = subprocess.run(['python3', '-I', str(ffx / 'gen_shaderblobs.py'), '/usr/bin/glslangValidator', str(ffx), str(blobs)],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:]); raise SystemExit('FidelityFX shader compilation failed')
    print('compiled FidelityFX shaders')
ffx_flags = ['-std=c++20', '-O2', '-g0', '-march=x86-64-v3', '-fno-exceptions', '-fno-strict-aliasing', '-DNDEBUG', '-DFFX_FI', '-DFFX_OF', '-w',
             '-I' + str(ffx / 'include'), '-I' + str(ffx / 'src'), '-I' + str(ffx / 'src/backends/shared'), '-I' + str(ffx / 'src/components'),
             '-I' + str(ffx / 'src/shared'), '-include', str(ffx / 'src/ffx_linux_compat.h')] + \
            [x for x in includes if 'Vulkan-Headers' in x or 'vulkan' in x.lower()]
ffx_objects = []
for src in [ffx / x for x in ffx_sources] + [blobs]:
    obj = ffx_out / (src.stem + '.o')
    ffx_objects.append(str(obj))
    if obj.exists() and obj.stat().st_mtime > src.stat().st_mtime and obj.stat().st_mtime > (ffx / 'src/ffx_linux_compat.h').stat().st_mtime:
        continue
    r = subprocess.run([compiler, *ffx_flags, '-c', str(src), '-o', str(obj)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:]); raise SystemExit(f'compile failed: {src}')
    print('compiled', src.relative_to(work) if src.is_relative_to(work) else src.name)
lb = block('build kyty_emulator:')
first, *_ = lb.splitlines()
inputs = shlex.split(first.split(': ', 1)[1])[1:]
inputs = inputs[:inputs.index('|')] if '|' in inputs else inputs
inputs = inputs[:inputs.index('||')] if '||' in inputs else inputs
lf = fields(lb)
rep = {f'CMakeFiles/kyty_emulator.dir/{s}.o': str(o) for s, o in objects.items() if s in sources}
missing = [k for k in rep if k not in inputs]
if missing: raise SystemExit(f'link inputs not found: {missing}')
inputs = [rep.get(x, x) for x in inputs] + [str(objects[s]) for s in new_sources] + ffx_objects
rsp = out / 'link.rsp'
rsp.write_text('\n'.join(shlex.quote(x) for x in inputs) + '\n' + lf.get('LINK_LIBRARIES', ''))
link_flags = shlex.split(lf.get('LINK_FLAGS', ''))
link_flags = [x.replace(str(build / 'kyty_emulator_clang_lld.map'), str(out / 'kyty.map')).replace('CMakeFiles/kyty_emulator.dir/link.d', str(out / 'link.d')) for x in link_flags]
# --out=NAME: another executable (development builds), so the launcher's build changes only when meant to.
out_name = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--out=')), 'kyty_emulator_auto_upscaler')
exe = here / out_name
tmp = exe.with_suffix('.new')
cmd = [compiler, *shlex.split(lf.get('FLAGS', '')), *link_flags, '@' + str(rsp), '-o', str(tmp)]
with (logs / 'link.log').open('w') as log:
    log.write(shlex.join(cmd) + '\n')
    r = subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT)
if r.returncode:
    print((logs / 'link.log').read_text()[-3000:]); raise SystemExit('link failed')
tmp.replace(exe)
sha = hashlib.sha256(exe.read_bytes()).hexdigest()
prod_sha = hashlib.sha256((prod / 'release/kyty_emulator').read_bytes()).hexdigest()
git = subprocess.run(['git', '-C', str(work), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
(here / 'build-manifest.json').write_text(json.dumps({'executable': str(exe), 'sha256': sha, 'worktree_head': git,
    'production_release_sha256_unchanged': prod_sha, 'compiled': sources + new_sources,
    'other_objects_from': str(build), 'fidelityfx': str(ffx)}, indent=2) + '\n')
print('linked', exe, sha)
