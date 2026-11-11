# SteamOS AppImage packaging and path audit

Date: 2026-11-11. Package: KytyPS5-BC250-2026.11.11-beta2-x86_64.

## Result

The inspected game fixes are included in the downloaded AppImage. A traced Yotei launch finds its archive on the mounted CachyOS partition and reaches the native GPU command rejection without filesystem operations against the former /home/david root. This does not establish gameplay compatibility on SteamOS.

The two new Minimal Mode corrections remain manual changes outside this downloaded AppImage. The installer integration in this branch must be packaged into a new release to make them reproducible on a fresh host.

## Package contents

- All entries in the extracted package's FILES.sha256 passed checksum validation.
- Direct reads of the AppImage SquashFS at offset 944632 matched the extracted AppRun, runtime-selector.sh, selector-relocated.sh, prepare-runtime.py, install-system.py, launcher/KytyPS5.dll, emulator/elf/kyty_emulator_native and emulator/elf/kyty_emulator_auto_upscaler byte for byte.
- No broken or absolute symlinks were found inside the extracted payload.
- The original /home/david strings in BUILD-MANIFEST.json are build provenance. The installer also contains original template strings that it substitutes at installation. Finding these strings is not evidence of runtime file access.
- The inspected native ELF has no RPATH or RUNPATH pointing into the original installation.

## Previously repaired Yotei issues

The November 7 investigation records identify two distinct corrections:

1. The initial infinite spinner was addressed through content/entitlement registration. The previous launch verification lists 15 GHOST2 registrations, including GHOST2APP0000000 and GHOST2BASE000000.
2. The later post-cutscene black fade was addressed by reporting a dimension-based NV12 output-frame requirement in VideoDec2, instead of a fixed 16 MiB minimum. At 1920 by 1088 bounds the corrected requirement is 3,346,432 bytes, allowing ten output frames instead of two.

The actual downloaded AppImage was run with its selector dry-run option, a valid mounted Yotei archive, and isolated writable state. No forced title marker was supplied. It detected the title from metadata, selected its mounted native backend and passed all 15 registrations.

The fixed QueryDecoderMemoryInfo sizing arithmetic and CreateDecoder sizing/validation instruction sequences were matched against the preserved repaired libVideoDec2 object. Both sequences were found once in each of the packaged native and auto-upscaler ELF files. The corresponding ELF files inside the AppImage match those inspected files.

These checks substantiate the presence of those two fixes. They are not an exhaustive certification of every historical experiment or all game paths.

## Runtime path trace

A normal AppImage dry run succeeded. Tracing the AppImage runtime itself prevented its FUSE helper from mounting because tracing suppresses the helper's privilege transition. That instrumented failure is not treated as a product failure.

File tracing therefore used the verified matching extracted payload via AppRun --runtime, with isolated config/profile/data roots under the audit directory. The game archive was successfully opened at:

`/mnt/cachyos/@home/david/Downloads/Ghost of Yōtei Complete Edition/Ghost of Yōtei Complete Edition.zar`

The selected archive member was:

`UP9000-PPSA26344_00-GHOST2SHIP000000-files`

Before the deliberately bounded trace ended, no filesystem operations referenced an absolute /home/david path. Package scripts resolved the backend and assets within the downloaded package; writable runtime state resolved under the selected data root. The emulator then reported the same COND_EXEC rejection and waiting queues seen in the user's logs.

Failed lookups include normal PATH/library search candidates and optional Vulkan configuration directories. Such a failure is not sufficient evidence of a missing dependency when a later candidate succeeds.

Filesystem tracing observes host file accesses. It does not reveal failed member lookups inside an already opened ZArchive. Therefore it does not rule out an archive/VFS-specific resource lookup problem at a later loading stage.

## User state and partition changes

The current launcher process exports package paths under /home/deck/Downloads and config/data paths under /home/deck. It does not point its emulator wrapper at the old developer installation.

However, launcher.json still has LastGame and a GameFolders entry under the earlier /run/media/deck/... mount. Two stored upscaler profile records also retain that archive location; index.json contains both old mount entries and newer /var/mnt/cachyos entries. The old LastGame path is unavailable, while the archive at /mnt/cachyos is accessible.

These stale entries can affect library discovery, recent-game selection or profile/cache matching. They should be migrated consistently with backups and metadata/stamp validation. They have not been shown to cause the native GPU rejection in launches that use the accessible archive path. User configuration was not rewritten during this audit.

Keeping game data on a different partition is supported by the launch path; that partition must be mounted when the game is opened. This is separate from an emulator depending on its former installation.

## Remaining game failure

The normal Beta route rejects opcode 0x22 (COND_EXEC form) and drops the remainder of an already partially submitted DCB. Audit logging captured:

`c0032200 00000080 00000050 00000000 00000040`

Disabling chunking in a temporary diagnostic launcher allows whole-buffer fallback, plays the intro and continues producing frames. It is not a complete validated fix. An early direct-backend trial omitted the selector's registrations and remained at a loading indicator; that trial must not be used to claim the packaged registration fix is absent. A corrected trial preserved all 15 registrations and still produced many COND_EXEC refusals. It exited with code 0 after 141 seconds; whether the window was closed by the user was not confirmed.

The working CachyOS records use a different kernel/AMDGPU setup and often the upscaler backend. SteamOS logs report stock AMDGPU queue parameters and missing Vulkan fragmentShaderBarycentric. Those differences require targeted diagnosis rather than attributing the failure to partition relocation.

No working installation, GPU driver, kernel settings, existing saves or launcher configuration was patched during this audit. Diagnostic launches created logs and runtime cache/state through normal emulator operation.

## Local evidence

Evidence remains under /home/deck/kyty-steamos-fix:

- payload-checksums.log
- appimage-path-audit/actual-appimage-dry-run.log
- appimage-path-audit/payload-selector-dry-run.log
- appimage-path-audit/payload-files.log
- appimage-path-audit/game-files.log
- appimage-path-audit/game-runtime.log
- yotei-audit-chunked.log
- yotei-audit-whole.log
- yotei-audit-selector.log

Raw game/desktop captures and full trace files were not uploaded to GitHub.
