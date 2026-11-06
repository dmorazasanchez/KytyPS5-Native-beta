#!/usr/bin/env bash
# Execute only synthetic shaders lowered to known gfx1013 instructions.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
out="$root/_Build/instruction-tests"
mkdir -p "$out"
cxx="${CXX:-clang++}"
cc="${CC:-cc}"
sources=(src/graphics/native_gpu/shaderIntegerPatcher.cpp
         src/graphics/native_gpu/shaderPatcher.cpp
         src/graphics/native_gpu/shaderScan.cpp)
"$cxx" -std=c++20 -O2 -Isrc tools/bc250-native-probe/patch_integer_blob.cpp "${sources[@]}" -o "$out/patch-integer-blob"
"$cxx" -std=c++20 -O2 -Isrc tests/NativeGpuIntegerPatcherTests.cpp "${sources[@]}" -o "$out/integer-patcher-tests"
"$out/integer-patcher-tests"
read -ra drm_flags <<< "$(pkg-config --cflags --libs libdrm libdrm_amdgpu)"
"$cc" -std=gnu11 -O2 tools/bc250-native-probe/integer_patch_probe.c -o "$out/integer-patch-probe" "${drm_flags[@]}" -lm
python3 tools/bc250-native-probe/test_integer_patches.py
python3 tools/bc250-native-probe/test_addtid_patches.py
python3 tools/bc250-native-probe/test_dot_operand_patches.py
