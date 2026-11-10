#!/usr/bin/env bash
# Builds hlx_server.exe(.so), the Winelib HLX helper, with the system's wineg++ (64-bit). The FidelityFX API headers
# come from deps/ffx-2.4.0 (MIT), the NGX DLSS headers from the HelixSR-Scaler checkout (NVIDIA DLSS SDK, MIT).
set -euo pipefail
cd "$(dirname "$0")"
H=deps/ffx-2.4.0
N=deps/nvngx_dlss_sdk
wineg++ -m64 -DWIDL_EXPLICIT_AGGREGATE_RETURNS -std=c++20 -O2 -I$H/upscalers/include -I$H/api/include -Ideps/vulkan-headers -I$N \
  hlx_server.cpp -o hlx_server.exe -ld3d12 -ldxgi -luuid -ldl
echo "built $(sha256sum hlx_server.exe.so | cut -c1-16)"
