#!/usr/bin/env bash
# Synthetic native programs only. Python hardware helpers refuse active games.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
python3 - <<'PY'
import sys
sys.path.insert(0,'tools/bc250-native-probe')
from test_atomic_patches import ensure_idle
ensure_idle()
PY
bash tools/bc250-native-probe/run_integer_patch_tests.sh
out="$root/_Build/instruction-tests"
cxx="${CXX:-clang++}"
sources=(src/graphics/native_gpu/shaderIntegerPatcher.cpp
         src/graphics/native_gpu/shaderPatcher.cpp
         src/graphics/native_gpu/shaderScan.cpp)
"$cxx" -std=c++20 -O2 -Isrc tests/NativeGpuAtomicPatcherTests.cpp "${sources[@]}" -o "$out/atomic-patcher-tests"
"$out/atomic-patcher-tests"
python3 tools/bc250-native-probe/test_atomic_patches.py
"$cxx" -std=c++20 -O2 -Isrc tools/bc250-native-probe/patch_float_blob.cpp src/graphics/native_gpu/shaderFloatPatcher.cpp src/graphics/native_gpu/shaderPatcher.cpp src/graphics/native_gpu/shaderScan.cpp -o "$out/patch-float-blob"
python3 tools/bc250-native-probe/test_float_model.py
python3 tools/bc250-native-probe/test_float_hardware.py
