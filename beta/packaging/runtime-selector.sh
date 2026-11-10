#!/usr/bin/env bash
set -euo pipefail
bundle_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export KYTYPS5_BUNDLE_ROOT="$bundle_root"
export KYTYPS5_DATA_ROOT="${KYTYPS5_DATA_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/KytyPS5-Beta}"
export KYTYPS5_CONFIG_ROOT="${KYTYPS5_CONFIG_ROOT:-${XDG_CONFIG_HOME:-$HOME/.config}/KytyPS5-Beta}"
export KYTYPS5_PROFILE_ROOT="${KYTYPS5_PROFILE_ROOT:-$KYTYPS5_CONFIG_ROOT/profiles}"
export KYTYPS5_UPSCALER_PROFILE_DIR="${KYTYPS5_UPSCALER_PROFILE_DIR:-$KYTYPS5_CONFIG_ROOT/upscaler-profiles}"
export KYTYPS5_UPSCALER_SCANNER="$bundle_root/tools/upscaler-scan/kyty-upscaler-scan"
export PATH="$bundle_root/bin:$PATH"
export PYTHONDONTWRITEBYTECODE=1
"$bundle_root/bin/python3" "$bundle_root/prepare-runtime.py" --init
exec "$bundle_root/selector-relocated.sh" "$@"
