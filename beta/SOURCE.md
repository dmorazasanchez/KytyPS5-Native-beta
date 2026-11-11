# KytyPS5 beta source snapshot

## Attribution and project lineage

This project is a downstream fork of **[Superkeka's KytyPS5-Native](https://github.com/superkeka/KytyPS5-Native)**. Superkeka and its contributors created the emulator's main native BC-250 implementation. It also descends from the original [KytyPS5](https://github.com/KytyPS5/KytyPS5).

**[Lonewolf's HelixSR](https://github.com/lonewolf0622/HelixSR)** provides the separately developed HelixSR technology used by this fork's optional HLX upscaling integration. Neither HelixSR nor its required proprietary/vendor runtime assets are represented as this fork's original work. Credit their authors and respect their licensing and installation instructions.

`launcher/` is the exact staged launcher source used for this beta. Build with
.NET SDK 10 (`dotnet publish src/KytyPS5.Launcher/KytyPS5.Launcher.csproj -c Release`).
`packaging/` contains the complete release/installation/relocation scripts.

The repository root contains the emulator source from `aa1dff5`, including the
shared VMID and Scream recovery fixes. Its Git submodules pin third-party source.
The release source archive also includes a snapshot of those dependencies.
`variant-overlays/` preserves the source overlays for the previously integrated
Astro and recovery/audio variants; `integration-records/` captures their build
recipes, source hashes, compile configuration and final binary hashes. Those
records retain original developer build paths and incremental link history;
they are provenance records, not a claim that an old object-cache path exists
on a recipient's machine. Runtime packages contain no references to those paths.

Beta 1 packaged the retained runtime binaries byte for byte. In Beta 2,
`packaging/relink-beta2-backends.py` reproduces the original hashes for five
unique backends, then relinks the same objects against the libm from Ubuntu
24.04. The original Fast backend uses `packaging/rebind-legacy-libm.py` to
bind seven imports to their existing older versions. The emulator resolves
libm from the host at runtime. `packaging/check-native-abi.py` enforces the
glibc 2.38 baseline. The launcher defaults to the Upstream backend.

The bundled session runtime comes from Debian 13 packages. Build the image with
`packaging/Dockerfile.minimal`, then run `packaging/export-session-runtime.sh`
in the image with a writable `/output` mount. Place the exported directory at
`downloads/session-runtime` for packaging. The installed files include the
package versions and copyright notices. [Debian labwc source package](https://packages.debian.org/source/trixie/labwc)

Portable upstream runtime URLs and verified hashes are in runtime-downloads.json.
Proton/Wine source: https://github.com/CachyOS/proton-cachyos (packaged version
cachyos-11.0-20261005-slr); original notices are in the binary package.
The package excludes Helix-generated neural weights/shaders and the proprietary
Lossless DLL. Each tester prepares/imports their own files.

## Public-source exception: NVIDIA NGX headers

NVIDIA NGX SDK headers formerly under beta/hlx-helper/deps/nvngx_dlss_sdk/
were labeled LicenseRef-NvidiaProprietary and prohibit redistribution
without an express NVIDIA license. They are not part of the public source
or release archives. Build the optional HLX helper using vendor headers
that you obtained independently under appropriate licensing.
