#!/usr/bin/env python3
"""Import a tester's own prepared runtime. Does not download proprietary material."""
import argparse, json, os, shutil
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--helix-release', type=Path, help='Your locally prepared complete HelixSR 3.2.0 release directory')
p.add_argument('--lossless-dll', type=Path, help='Your own Lossless Scaling 3.2.2 DLL')
p.add_argument('--mako', type=Path, help='Your MAKO Renderer 4.6.0 installation/archive directory')
a = p.parse_args()
if not any((a.helix_release, a.lossless_dll, a.mako)): p.error('Choose an import option; see --help')
data = Path(os.environ['KYTYPS5_DATA_ROOT'])
if a.helix_release:
    release = a.helix_release.expanduser().resolve()
    files = {'HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll':'hlx/amd_fidelityfx_upscaler_dx12.dll', 'HelixSR/helixsr.dll':'hlx-dlss/helixsr.dll', 'HelixSR/helixsr.ini':'hlx-dlss/helixsr.ini'}
    for src in files:
        if not (release / src).is_file(): p.error('Missing ' + str(release / src) + '; run the official Helix setup first')
    # The same official setup marker that produced the tested HLX package.
    if not (release / 'HLX-FSR4/HLX-FSR4-built.txt').is_file(): p.error('HLX local build marker missing; run the official setup first')
    for src, dst in files.items():
        target = data / 'upscalers/dlls' / dst
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(release / src, target)
    for name in ('LICENSE', 'SOURCE.md', 'THIRD_PARTY_NOTICES.md'):
        if (release / name).exists(): shutil.copy2(release / name, data / 'upscalers' / name)
    print('Imported your locally prepared Helix runtime into ' + str(data / 'upscalers'))
if a.lossless_dll:
    dll = a.lossless_dll.expanduser().resolve()
    if not dll.is_file() or dll.open('rb').read(2) != b'MZ': p.error('Lossless DLL is not a Windows DLL')
    dest = Path.home() / '.local/share/kyty-lsfg/Lossless-3.2.2.dll'
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, dest)
    print('Imported your Lossless Scaling DLL.')
if a.mako:
    root = a.mako.expanduser().resolve()
    if not (root / 'lib/libmako-render.so').is_file(): p.error('MAKO lib/libmako-render.so missing')
    dest = Path.home() / '.local/opt/kyty-lsfg-dynamic-4.6.0'
    if root != dest.resolve(): shutil.copytree(root, dest, dirs_exist_ok=True, symlinks=True)
    manifest = dest / 'share/vulkan/implicit_layer.d/VkLayer_MAKO_render.json'
    manifest.parent.mkdir(parents=True, exist_ok=True)
    original = root / 'share/vulkan/implicit_layer.d/VkLayer_MAKO_render.json'
    if not original.exists(): original = root / 'share/mako-render/vulkan/implicit_layer.d/VkLayer_MAKO_render.json'
    content = json.loads(original.read_text())
    content['layer']['library_path'] = str(dest / 'lib/libmako-render.so')
    manifest.write_text(json.dumps(content, indent=2) + '\n')
    config = Path.home() / '.config/lsfg-vk/kyty-yotei-dynamic-60.toml'
    config.parent.mkdir(parents=True, exist_ok=True)
    dll = str(Path.home() / '.local/share/kyty-lsfg/Lossless-3.2.2.dll')
    config.write_text('version = 2\n[global]\ndll = ' + json.dumps(dll) + '\nallow_fp16 = true\n[[profile]]\nname = "Yotei Dynamic 60 FPS"\nframe_generation_provisioned = true\nframe_generation_enabled = true\nadaptive = true\ntarget_fps = 60\nadaptive_max_multiplier = 2\nadaptive_auto_base_fps_cap = false\nadaptive_stable_cadence = false\nbase_fps_cap = 60\ndynamic_cadence_recovery = false\ngpu_headroom_percent = 0\nscaling_enabled = false\nflow_scale = 0.50\nperformance_mode = true\npacing = "none"\n')
    print('Prepared Yotei Adaptive 60 FPS MAKO configuration. Other LSFG modes use your existing fixed/experimental LSFG installation.')
