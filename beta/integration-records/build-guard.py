"""Retain the existing recovery/audio backend while adding the shared VMID fix."""
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import shutil
import subprocess

audit = Path(__file__).resolve().parent
work = audit.parents[1]
old_source = Path('/home/david/kyty-published-upstream-b4496cc-20261103/source-b4496')
build = old_source.parent / 'build-b4496-guard'
overlay = audit / 'guard-source'
parser = argparse.ArgumentParser()
parser.add_argument('--out-dir', type=Path, default=work / 'release')
args = parser.parse_args()
release = args.out_dir
release.mkdir(parents=True, exist_ok=True)
for rel in ('src/graphics/native_gpu/nativeShadow.cpp', 'src/graphics/native_gpu/nativeSubmit.h', 'src/loader/runtimeLinker.cpp'):
    p = overlay / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(old_source / rel, p)
shutil.copy2(work / 'src/graphics/native_gpu/nativeVmid.h', overlay / 'src/graphics/native_gpu/nativeVmid.h')
shutil.copy2(work / 'src/loader/demonsScreamRecovery.h', overlay / 'src/loader/demonsScreamRecovery.h')
p = overlay / 'src/loader/runtimeLinker.cpp'
text = p.read_text().replace('#include "common/hostException.h"', '#include "common/hostException.h"\n#include "loader/demonsScreamRecovery.h"', 1)
text = text.replace('\tconst auto* info = &exception_info;', '\tconst auto* info = &exception_info;\n#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__x86_64__)\n\tif (DemonsScreamRecovery::TryRecoverDemonsScreamAssert(info)) return true;\n#endif', 1)
text = text.replace('\tif (!Common::HostException::InstallHandler(KytyExceptionHandler)) {', '#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && defined(__x86_64__)\n\tDemonsScreamRecovery::ConfigureFromEnvironment();\n#endif\n\tif (!Common::HostException::InstallHandler(KytyExceptionHandler)) {', 1)
p.write_text(text)


main = (work / 'src/graphics/native_gpu/nativeShadow.cpp').read_text()
p = overlay / 'src/graphics/native_gpu/nativeShadow.cpp'
text = p.read_text().replace('#include "graphics/native_gpu/nativeSubmit.h"',
                           '#include "graphics/native_gpu/nativeVmid.h"\n#include "graphics/native_gpu/nativeSubmit.h"', 1)
a = text.index('\t// The per-VMID PS5 settings follow')
b = text.index('\n\t// The ACB translator', a)
text = text[:a] + text[b:]
text = text.replace('\t\tShadow::Instance().Ps5RegsOnFlip();\n', '')
a = main.index('\t// Read through the existing helper API;')
b = main.index('\t// Applies the helper', a)
method = main[a:b].replace('ReadAbs(0x10a0u + vmid, &lut[vmid])', 'ReadReg(0x10a0u + vmid, 0, &lut[vmid])')
needle = '\t// Applies the helper\'s local golden table'
assert needle in text
text = text.replace(needle, method + needle, 1)
a = main.index('\tvoid InitPs5Regs() {')
b = main.index('\n\t// One PS5 setting:', a)
init = main[a:b]
a = text.index('\tvoid InitPs5Regs() {')
b = text.index('\n\t// One PS5 setting:', a)
text = text[:a] + init + text[b:]
text = text.replace('\tuint32_t              m_ps5_gfx_vmid = 0; // the gfx VMID the per-VMID settings are on now\n', '')
assert 'Ps5RegsOnFlip' not in text and 'm_ps5_gfx_vmid' not in text
p.write_text(text)

main = (work / 'src/graphics/native_gpu/nativeSubmit.h').read_text()
a = main.index('\t// Native PS5 per-VMID setup:')
b = main.index('\n\tResult Submit(', a)
method = main[a:b]
# This older backend predates the named CP-shadow tail. Use the identical
# eight words verified by the newer native backend, without game shaders.
method = method.replace('const Result prime = Submit(kCpShadowOff, {}, 0.5);',
    'constexpr std::array<uint32_t, 8> prime_tail {0xc0012800u, 0x80000000u, 0x80000000u, '
    '0xffff1000u, 0xffff1000u, 0xffff1000u, 0xffff1000u, 0xffff1000u};\n'
    '\t\tconst Result prime = Submit(prime_tail, {}, 0.5);')
p = overlay / 'src/graphics/native_gpu/nativeSubmit.h'
text = p.read_text()
assert text.count('\n\tResult Submit(') == 1
p.write_text(text.replace('\n\tResult Submit(', '\n' + method + '\n\tResult Submit(', 1))

blocks = (build / 'build.ninja').read_text().split('\n\n')
def block(prefix):
    return next(x for x in blocks if x.startswith(prefix))
def fields(value):
    return dict(line.strip().split(' = ', 1) for line in value.splitlines()[1:] if ' = ' in line)

base = fields(block('build CMakeFiles/kyty_emulator.dir/src/graphics/native_gpu/nativeShadow.cpp.o:'))
obj = audit / 'nativeShadow-guard.o'
command = ['/usr/bin/clang++', *shlex.split(base['DEFINES']), *shlex.split(base['FLAGS']),
           '-I' + str(overlay / 'src'), *shlex.split(base['INCLUDES']),
           '-c', str(overlay / 'src/graphics/native_gpu/nativeShadow.cpp'), '-o', str(obj)]
with (audit / 'logs/nativeShadow-guard-build.log').open('w') as log:
    log.write(shlex.join(command) + '\n')
    log.flush()
    subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)

runtime_base = fields(block('build CMakeFiles/kyty_emulator.dir/src/loader/runtimeLinker.cpp.o:'))
runtime_obj = audit / 'runtimeLinker-guard.o'
command = ['/usr/bin/clang++', *shlex.split(runtime_base['DEFINES']), *shlex.split(runtime_base['FLAGS']),
           '-I' + str(overlay / 'src'), *shlex.split(runtime_base['INCLUDES']),
           '-c', str(overlay / 'src/loader/runtimeLinker.cpp'), '-o', str(runtime_obj)]
with (audit / 'logs/runtimeLinker-guard-build.log').open('w') as log:
    log.write(shlex.join(command) + '\n'); log.flush()
    subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)

lb = block('build kyty_emulator:')
lf = fields(lb)
inputs = shlex.split(lb.splitlines()[0].split(': ', 1)[1])[1:]
for marker in ('|', '||'):
    if marker in inputs:
        inputs = inputs[:inputs.index(marker)]
inputs += shlex.split(lf.get('LINK_LIBRARIES', ''))

def link(name, items):
    rsp = audit / (name + '.rsp')
    rsp.write_text('\n'.join(shlex.quote(x) for x in items) + '\n')
    flags = [x.replace(str(build / 'kyty_emulator_clang_lld.map'), str(audit / (name + '.map')))
             .replace('CMakeFiles/kyty_emulator.dir/link.d', str(audit / (name + '-link.d')))
             for x in shlex.split(lf.get('LINK_FLAGS', ''))]
    output = release / name
    command = ['/usr/bin/clang++', *shlex.split(lf.get('FLAGS', '')), *flags, '@' + str(rsp), '-o', str(output)]
    with (audit / 'logs' / (name + '-link.log')).open('w') as log:
        log.write(shlex.join(command) + '\n')
        log.flush()
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    return hashlib.sha256(output.read_bytes()).hexdigest()

baseline = link('guard-audio-relinked-baseline', inputs)
expected = hashlib.sha256(Path('/home/david/kyty-audio-dialogue-test-20261105/kyty_emulator_audio_dialogue_test').read_bytes()).hexdigest()
assert baseline == expected, 'Recovery/audio baseline did not reproduce; do not deploy.'
target = 'CMakeFiles/kyty_emulator.dir/src/graphics/native_gpu/nativeShadow.cpp.o'
assert target in inputs
replacements = {target: str(obj), 'CMakeFiles/kyty_emulator.dir/src/loader/runtimeLinker.cpp.o': str(runtime_obj)}
sha = link('kyty_emulator_guard', [replacements.get(x, x) for x in inputs])
shutil.copy2(release / 'kyty_emulator_guard', release / 'kyty_emulator_audio')
(audit / 'guard-build-manifest.json').write_text(json.dumps({
    'baseline_sha256': baseline, 'baseline_matches_existing_audio_backend': True,
    'sha256': sha, 'changed_objects': [target, 'CMakeFiles/kyty_emulator.dir/src/loader/runtimeLinker.cpp.o'], 'game_launched': False,
}, indent=2) + '\n')
print('Linked shared recovery/audio backend with general native VMID setup:', sha)
