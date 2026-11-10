#!/usr/bin/env python3
from pathlib import Path
import re
base = Path(__file__).resolve().parent.parent
out = base / 'work/KytyPS5-Beta.AppDir'
source = Path('/home/david/sharpemu-upscaling-20261108/kytyps5-runtime.sh').read_text()
source = source.replace('set -euo pipefail', 'set -euo pipefail\nbundle_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"\nbeta_data="${KYTYPS5_DATA_ROOT:?}"', 1)
source = source.replace('mode="${KYTY_NATIVE_GPU_PM4_MODE:-fast}"', 'mode="${KYTY_NATIVE_GPU_PM4_MODE:-upstream}"')
source = re.sub(r'/home/david/kyty-native-vmid-20261110/release/20261110-recovery/(kyty_emulator_\w+)', r'"$bundle_root/emulator/\1"', source)
source = source.replace('/home/david/kyty-upstream-clean-47857b4-20261104/release', '"$beta_data/runtime"')
source = source.replace('/home/david/kyty-native-release-160251b-20261029/kyty_emulator_demons1007_screamguard_6d3e578b', '"$bundle_root/emulator/kyty_emulator_fast_guard"')
source = source.replace('/home/david/kyty-native-release-160251b-20261029/kyty_emulator', '"$bundle_root/emulator/kyty_emulator_fast"')
source = source.replace('/home/david/kyty-auto-upscaler-20261109/tools/upscaler-scan/kyty-upscaler-scan', '$bundle_root/tools/upscaler-scan/kyty-upscaler-scan')
source = source.replace('/home/david/sharpemu-upscaling-20261108/kyty-yotei-lsfg-dynamic.sh', '"$bundle_root/kyty-yotei-lsfg-dynamic.sh"')
source = source.replace('/home/david/kyty-auto-upscaler-20261109/experiments/auto-upscaler/logs', '"$beta_data/logs"')
source = source.replace('mkdir -p /home/david/kyty-native-upscaling-20261108/experiments/native-upscaling/logs', 'mkdir -p "$beta_data/logs"')
source = source.replace('/home/david/kyty-native-upscaling-20261108/experiments/native-upscaling/logs', '$beta_data/logs')
start = source.index('content_title="$scream_title"')
end = source.index('# Explicitly scoped Vulkan-layer injection.', start)
source = source[:start] + '''content_title="$scream_title"
if [[ "$mode" == upstream && -z "$content_title" ]]; then
    game_arg= prev=
    for a in "$@"; do
        if [[ "$prev" == --game ]]; then game_arg="$a"; fi
        prev="$a"
    done
    if [[ -n "$game_arg" ]]; then
        content_title="$(python3 -I "$bundle_root/prepare-runtime.py" --title "$game_arg")"
    fi
fi

''' + source[end:]
# Rebind Astro's manual profile by metadata and executable SHA on the tester's archive/folder.
source = source.replace('astro_root=/home/david/astro-helix-framegen-20261110/manual', '''astro_game_arg= prev=
for a in "$@"; do
    if [[ "$prev" == --game ]]; then astro_game_arg="$a"; fi
    prev="$a"
done
astro_root="$beta_data/astro/not-matched"
if [[ "$up_build" == 1 && "$mode" == upstream && "$content_title" == PPSA21567 ]]; then
    bound="$(python3 -I "$bundle_root/prepare-runtime.py" --astro "$astro_game_arg")" || exit 2
    if [[ -n "$bound" ]]; then astro_root="$bound"; fi
fi''')
start = source.index("    if python3 -I - \"$astro_game_arg\"")
end = source.index('CHECK_ASTRO\n', start) + len('CHECK_ASTRO\n')
source = source[:start] + '    if [[ -f "$astro_root/astro-manual.json" ]]\n' + source[end:]
# The original environment filter strips KYTY_HLX_*. Export the rebased paths only after it.
marker = '# Log the final choice after every backend override.'
source = source.replace(marker, '''export KYTY_HLX_HOME="$beta_data/upscalers" KYTY_HLX_PROTON="$bundle_root/proton/files"
if [[ "${KYTY_UPSCALER:-game}" == hlx_* || "${KYTY_UPSCALER:-game}" == auto ]]; then
    if [[ ! -f "$beta_data/upscalers/dlls/hlx/amd_fidelityfx_upscaler_dx12.dll" || ! -f "$beta_data/upscalers/dlls/hlx-dlss/helixsr.dll" ]]; then
        echo 'Helix runtime is not prepared. Use --import-upscalers --helix-release /path/to/your/locally-built/HelixSR-release, or select Game upscaling.' >&2
        exit 2
    fi
fi
''' + marker)
# Fingerprint the actual ELF, not its relocation wrapper.
source = source.replace('sha256sum -- "$selected"', 'sha256sum -- "$bundle_root/emulator/elf/${selected##*/}"')
if '/home/david' in source: raise RuntimeError('Unrebased selector path')
(out / 'selector-relocated.sh').write_text(source)
(out / 'selector-relocated.sh').chmod(0o755)
dynamic = Path('/home/david/sharpemu-upscaling-20261108/kyty-yotei-lsfg-dynamic.sh').read_text()
(out / 'kyty-yotei-lsfg-dynamic.sh').write_text(dynamic)
(out / 'kyty-yotei-lsfg-dynamic.sh').chmod(0o755)
