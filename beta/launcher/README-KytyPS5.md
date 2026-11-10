# KytyPS5 launcher

A standalone Avalonia desktop launcher for the current native Kyty build and
native-upscaling selector. It has no project references to the SharpEmu runtime,
GUI, HLE, shader compiler, debugger or updater. The only runtime dependencies are
the .NET 10 desktop libraries, Avalonia/Skia and the system SDL2 controller library.
There is no web engine, video background, blur pass or continuously animated UI.
The integrated music/effects system uses the same system SDL2 library; FFmpeg is
used only for one-time local music conversion/import. See [Audio notes](AUDIO-KytyPS5.md)
for installed music sources, independent volume controls and verification.
Artwork is decoded to 256-pixel covers and one 1600-pixel background. Input polling
slows while a game is running; inactive-window input is ignored.

## Start and build

```sh
./run-kytyps5.sh
dotnet publish src/KytyPS5.Launcher/KytyPS5.Launcher.csproj -c Release \
  --self-contained false -o artifacts/kytyps5
./install-kytyps5.sh --no-build
```

Installation updates the existing PS5 session launcher and application entry to
KytyPS5, adds a desktop shortcut, and makes the PS5 Lite supervisor recognize the
new window class. Replaced files are copied to `artifacts/install-backups/`.
The previous SharpEmu source and executable remain available for rollback.

## Controller

| Input | Action |
| --- | --- |
| D-pad / left stick | Navigate |
| Cross / A | Play, select, toggle or enter text |
| Circle / B | Back; from a setting, return to its category |
| Triangle / Y | Selected game's settings |
| Square / X | Library |
| L1 / R1 | Switch page; in Settings, switch category |
| Left / right on a setting | Change a choice |
| PS / Guide / Menu | Control centre |
| L1 / R1 in the on-screen keyboard | Delete character / focus Save |

Keyboard equivalents are arrows, Enter, Escape, F3, F4, Q/E and F10. F11 toggles
fullscreen. The controller can also use the folder/file browser, cheats, text
entry, profile switching and the control centre. A running game's input belongs
to its own window; its existing PS/F10 menu remains responsible for live runtime
upscaling and cheat controls.

## Settings compatibility

All 74 native setting definitions, plus custom arguments, custom environment,
version-checked cheat selection and controller mappings are retained. The new
launcher copies the existing Kyty command builder and selector rather than changing
renderer behavior. Global and per-game profiles continue to use
`~/.config/kyty-sharp-frontend/*.json`, including unknown fields. Existing Qt
preferences are imported read-only. Saves and game content keep their existing
locations. Native upscaling support remains whatever the current runtime provides:
the validated Ghost of Yōtei build in Upstream mode.

Launcher preferences live separately in `~/.config/KytyPS5/launcher.json`; game
folders, exclusions and title music preferences import from the previous launcher
on first use. Only the setting being edited is written, so a game continues to
inherit future global changes for settings it has not overridden. Changes apply
at the next launch. Profile resets retain timestamped backups.

The library and Add games browser accept both extracted games and `.zar` files,
including archives that contain a single game subfolder. Only title metadata,
artwork and title music are cached under the launcher preferences; the game runs
directly from its archive. `./run-kytyps5.sh --list-games` prints the scanned library.

## Verification

`./run-kytyps5.sh --verify` checks metadata, library scanning, all settings,
global/per-game inheritance, launch arguments, upstream filtering, upscaling,
frame generation, exact-version patch matching, SDL button mapping, analog input,
disconnects and background input suppression. SDL mapping uses a temporary virtual
controller and does not send input to real devices.

The opt-in UI integration check uses `KYTYPS5_UI_SMOKE_DIR` and requires separate
`KYTYPS5_CONFIG_ROOT`, `KYTYPS5_PROFILE_ROOT` and a runtime stub under the smoke
directory. It visits every settings row through the controller navigation path,
checks text entry and folder browsing, renders previews and exercises a simulated
game session. It never boots a game. Results and previews for this build are under
`artifacts/gui-verification/`. Launch arguments and environment were also compared
with the previous frontend for both installed titles and were identical.

The launcher is covered by the repository's GPL-2.0-or-later license. The original
command builder and cheat implementation retain their source attribution.

## Yōtei: Lossless Scaling and cheats

The installed Yōtei 02.002.000 profile uses Adaptive 60 FPS Lossless Scaling,
with its 4K HLX FSR4 Performance upscaling retained. The private MAKO Renderer
4.6.0 backend reads the user's existing Lossless 3.2.2 DLL. It uses fractional
Adaptive generation, a 60 FPS real-frame cap, a 2× ceiling, the lighter model,
and 50% Flow Scale. Steady Base Cap and stable-cadence reduction are disabled,
so it does not deliberately lock the game to 30 real FPS. Fixed 2× and Off remain
available in the same menu. Other games retain their previous presentation backend.

Both selectors use the newer `kyty_emulator_framegen` build. With Lossless Scaling
selected, native FSR3 frame generation starts Off. The newer build sizes HLX
input textures to the Performance render extent, saving approximately 107 MiB
against the previous binary. No native FSR3 FG source was edited for this change.

Adaptive settings are in `~/.config/lsfg-vk/kyty-yotei-dynamic-60.toml`; the backend
is in `~/.local/opt/kyty-lsfg-dynamic-4.6.0`. On this single-GPU host the launch
bypasses Mesa's device-selection layer, whose instance creation otherwise crashes
with this renderer. App presentation stays Immediate; the renderer uses FIFO for
its ordered output. Its 60 FPS target is not a guarantee through stalls or GPU load.

A 100-second 4K HLX gameplay run completed without a GPU watchdog stall, with
120–346 MiB available after startup (median 206 MiB). The scheduler accepted
roughly 44–46 real FPS plus generated frames toward 60. Game FSR3 left more
headroom; switching to HLX DLSS drove available memory below 100 MiB and the test
stopped automatically. These runs used Developer Mode, not the installed minimal
Gaming Mode. Gaming Mode's proposed additional memory headroom remains unmeasured.
The gaming boot entry already routes to this launcher. Steam is not involved.

Evidence, backups and selector routing checks:
`/home/david/yotei-lsfg-dynamic-60-20261108/`.

Select Yōtei, open Game settings → Cheats, or use Settings → Cheats → Manage cheats.
Infinite Health, One-Hit Kill and Max All Resources retain their enabled states;
Uncapped FPS is enabled. Existing patch instructions are unchanged. UTF-8 files
with a byte-order marker now load correctly, and subsequent toggle saves use UTF-8
without a marker. Version checks and concurrent-edit protection remain in place.


## Demon's Souls: native TAA upscaling and frame generation

Game settings → Upscaling selects Game TAA, Helix FSR 4 or Helix DLSS (Model E), and native FSR 3 frame generation
(Off, Automatic or 2×). Supported TAA profiles now launch the validated TAA/FG build, including the HUD-free
capture and display-coordinate corrections. Automatic FG starts only below 50 real FPS. The shared-memory guard
can pause FG when the BC-250 has insufficient RAM or sustained memory pressure; its message updates when
Helix becomes ready. The Demon's Souls TAA/FG build no longer requires spare GPU capacity or a 256 MB GPU
reserve to enable, continue or resume FG. The PS / F10 menu continues to expose runtime choices.

Ultra Quality now renders at 2944×1656 and Helix reconstructs the final scene to 3840×2160. The final composite
samples that 4K image directly. The old DLAA route keeps its original frame-generation coordinate transform;
the lower-resolution composite uses native scene orientation and consecutive displayed-frame IDs. The user
confirmed the resulting motion, and saved real/generated/HUD-free images verify the corrected alignment.

The launcher passed 113 checks; end-to-end selection checks covered both Helix backends, native FG and Off.
Yotei keeps its existing FSR3/AGC route. The published Demon's Souls TAA/FG build is now
`007f0ebae0b35afa5b1e8fa097a9470556b3421acc485a4c4693390ecc8e66ab`.
