#!/usr/bin/env bash
set -euo pipefail
r="$1"
out="$2"
rm -rf -- "$out/config" "$out/profiles"
rm -f -- "$out/failure.txt" "$out/result.json"
mkdir -p "$out/config" "$out/profiles" "$out/game/sce_sys" "$out/data"
printf '%s\n' '{"titleId":"PPSA00000","contentVersion":"01.000.000","localizedParameters":{"en-US":{"titleName":"Beta packaging fixture"}}}' > "$out/game/sce_sys/param.json"
printf 'fixture\n' > "$out/game/eboot.bin"
printf '{"GameFolders":["%s/game"],"Fullscreen":false}\n' "$out" > "$out/config/launcher.json"
printf '#!/bin/sh\necho "Isolated runtime stub"\nsleep 2\n' > "$out/runtime-stub"
chmod +x "$out/runtime-stub"
export KYTYPS5_CONFIG_ROOT="$out/config" KYTYPS5_PROFILE_ROOT="$out/profiles" KYTYPS5_DATA_ROOT="$out/data"
export KYTYPS5_AUDIO_ROOT="$out/audio" KYTYPS5_UI_SMOKE_DIR="$out" KYTY_SHARP_EMULATOR="$out/runtime-stub"
export KYTYPS5_FFMPEG="$r/bin/ffmpeg" KYTYPS5_UPSCALER_SCANNER=/nonexistent
export DOTNET_ROOT="$r/dotnet" SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
export LD_LIBRARY_PATH="$r/runtime/lib"
exec timeout 60 xvfb-run -e "$out/xvfb.log" -a -s '-screen 0 1600x900x24 -extension GLX' "$r/dotnet/dotnet" "$r/launcher/KytyPS5.dll"
