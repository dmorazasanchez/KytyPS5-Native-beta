# KytyPS5 beta source snapshot

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

The current runtime binaries were packaged byte for byte, not rebuilt during
packaging. Legacy Fast rollback binaries retain their original behavior. The
launcher defaults to the current Upstream backend in this beta.

Portable upstream runtime URLs and verified hashes are in runtime-downloads.json.
Proton/Wine source: https://github.com/CachyOS/proton-cachyos (packaged version
cachyos-11.0-20261005-slr); original notices are in the binary package.
The package excludes Helix-generated neural weights/shaders and the proprietary
Lossless DLL. Each tester prepares/imports their own files.
