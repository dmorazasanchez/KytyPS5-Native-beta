# Third-party runtime inventory

Kyty native emulator: GPL-2.0-only, with original Kyty MIT notices. Source and
build/integration records accompany this beta. Launcher adaptation: GPL-2.0-or-later.

Official .NET runtime (Microsoft, MIT and its bundled THIRD-PARTY-NOTICES):
https://github.com/dotnet/runtime ; exact archive URL/hash in runtime-downloads.json.

Portable CPython 3.14 (PSF and bundled dependency licenses):
https://github.com/astral-sh/python-build-standalone ; archive metadata/hash in
runtime-downloads.json. Its license files are retained in python/.

SDL2 2.26.5 (zlib), Debian bookworm binary: https://www.libsdl.org/ ; Debian package
metadata is recorded in runtime-downloads.json. The host's SDL3 compatibility
library is deliberately not shipped.

GNU binutils (Debian bookworm) and C++ runtime libraries (CachyOS): GNU GPL with
relevant runtime exceptions. Versions/package source locations are recorded in
HOST-PACKAGES.txt. https://www.gnu.org/software/binutils/ and https://gcc.gnu.org/ .

CachyOS Proton/Wine 11.0 runtime: original Proton LICENSE/OFL retained.
https://github.com/CachyOS/proton-cachyos and https://github.com/ValveSoftware/Proton .
No user prefix, registry, Steam identity or shader caches are bundled.

FFmpeg 5.1.x and its runtime libraries: unmodified Debian bookworm packages,
GPL/LGPL and dependency licenses. Package inventory, exact version and notices
are in runtime/ffmpeg-runtime/. https://ffmpeg.org/ and https://sources.debian.org/ .

Avalonia, SkiaSharp, HarfBuzzSharp and ZstdSharp: resolved package/version inventory
in KytyPS5.deps.json; source repository and license notices are retained where
provided by packages. Launcher source references exact NuGet versions.

No Helix generated neural network/shader payloads, Lossless Scaling DLL, games,
console firmware, user saves or personal configuration are distributed here.
The MIT HLX IPC helper source is included separately. Optional MAKO and LSFG
components are imported from the tester's own installation, not bundled.
