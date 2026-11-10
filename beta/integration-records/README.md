# General native VMID fix installed in the main launcher

**Later correction:** an audit found that the recovery/audio baseline used here lacked the Scream assertion handler, despite the earlier retention claim below. The handler has now been restored in all launched backends, and archive-specific cheat/recovery associations repaired. The current release, checks and limitations are recorded in [the recovery/cheat audit](../recovery-cheats-audit-20261110/README.md). The rest of this file describes the earlier VMID integration.

The user's instruction was: “integrate the patch in the main launcher... make it general not game specific”. The main KytyPS5 launcher's runtime selector now uses builds with the graphics VMID fix enabled automatically in native startup. No title ID, game mode, shader hash or experimental enable flag controls this fix.

## Behavior

Native startup reserves a graphics-hub VMID, primes its PASID mapping without executing a game shader, sets the requested PS5 per-VMID registers, and verifies them before title GPU submissions. The per-flip aperture migration callbacks were removed. Failed reservation, ambiguous ownership or failed register readback stops startup with code 78 rather than continuing with unverified state.

The code is committed as **`1f4ca12e4fba6d056529ed93beeef6afab3b2293`** in `/home/david/kyty-native-vmid-20261110`, branch `fix/native-reserved-vmid-20261110`. See [native-vmid.patch](native-vmid.patch) and [the design note](/home/david/kyty-native-vmid-20261110/docs/native-gfx-vmid.md).

## Launcher integration

The existing desktop entry and frontend still invoke [kytyps5-runtime.sh](/home/david/sharpemu-upscaling-20261108/kytyps5-runtime.sh). Its regular Upstream routes now select these executables from `/home/david/kyty-native-vmid-20261110/release`:

| Existing route | Updated executable |
| --- | --- |
| Native renderer / unsupported upscaler profile | `kyty_emulator_native` |
| Profiled SDK upscaling and FG | `kyty_emulator_auto_upscaler` |
| Profiled TAA upscaling and FG | `kyty_emulator_launcher_taa_fg` |
| Built-in SDK fallback | `kyty_emulator_native_upscaler` |
| Existing manual Astro route | `kyty_emulator_astro` |
| Existing optional recovery route | `kyty_emulator_guard` |
| Existing optional audio test route | `kyty_emulator_audio` |

Distinct paths preserve the selector's existing profile/preset branches. The fix itself is shared across all these builds and applies to every game using them. Existing title-specific content registrations and upscaler profiles were not repurposed to enable the VMID fix. The explicitly selected legacy **Fast rollback** executables remain available unchanged.

SDK/TAA builds retain the committed launcher source at `1e72d273a8e1724a5c8c6ccbb1708389a602d425` plus the shared startup fix. The newer Astro implementation was kept separate: relinking its saved objects first reproduced its installed binary byte for byte (`a8bbf327e1a724741fb139a63777e91ce60c31431ce7fc8f30d3017e6d1edebb`), then only its native startup object was replaced. Its timing, upscaling, frame generation, profiles and presets are preserved.

The recovery/audio derivative similarly reproduced the existing combined recovery/audio backend (`6349d24f5f9ba76708918afa2b20fa3db17b9f845d59fe575052283cf0bd1013`) before replacing only its native startup object. Its existing audio opt-in remains present, and the selector still passes that option only on the audio route.

## Verification

- Header-only CPU watchdog/VMID checks passed with warnings treated as errors. They cover all graphics slots, multiple cached mappings, reassignment, zero/unknown PASIDs, and invalid/kernel/KFD masks.
- Native, SDK/TAA, Astro and recovery/audio builds linked successfully. Runtime library dependencies resolved for all four distinct binary variants.
- Ten dry-run routing/environment checks passed against both the staged and installed selector. They cover native defaults, unsupported profiles, SDK and TAA upscaling/FG, game TAA with FG, SDK fallback, optional recovery/audio, and Astro manual presets. Inherited diagnostic CU/VMID flags are stripped.
- Installed binary hashes and the selector content were verified. Frontend preferences, production saves, original executables, the GPU helper, services, CU settings and boot configuration were not changed by this integration.
- No game or GPU probe was launched during integration. The earlier separate reserved-VMID candidate completed its first bounded RT-configured run; these newly integrated variants have been compiled and checked on the CPU. Longer RT stability and other games' runtime behavior are not established by those checks.

Records: [installed.json](installed.json), [build-manifest.json](build-manifest.json), [guard-build-manifest.json](guard-build-manifest.json), [CPU results](cpu-tests.txt), [routing checks](routing-checks.json), [launcher patch](launcher-routing.patch). The build recipes and captured logs are in this directory.

## Rollback

The previous selector is preserved at:

`/home/david/sharpemu-upscaling-20261108/kytyps5-runtime.before-general-vmid-20261110.sh`

Restoring that selector restores the previous executable routing; the previous binaries were not overwritten. [rollback.py](rollback.py) performs that restoration only when invoked explicitly and refuses to overwrite subsequent selector changes.
