# KytyPS5 BC-250 beta — 2026.11.10-beta1

Controller-first Linux launcher, native BC-250 emulator, automatic upscaler discovery,
native FSR3 frame generation, exact-version render presets, Scream assertion recovery,
version-checked cheat controls, archive support and the optional minimal session.
The current VMID/recovery backend binaries are preserved byte for byte.

This is a beta candidate. Packaging checks do not establish gameplay stability on
Bazzite or SteamOS. Please include your distro/version with every report.

## First run

Use Desktop Mode for setup. Keep the downloaded file/folder in a permanent location.

AppImage:

```sh
chmod +x KytyPS5-BC250-2026.11.10-beta1-x86_64.AppImage
./KytyPS5-BC250-2026.11.10-beta1-x86_64.AppImage --doctor
./KytyPS5-BC250-2026.11.10-beta1-x86_64.AppImage --install-helper
./KytyPS5-BC250-2026.11.10-beta1-x86_64.AppImage --install-desktop
./KytyPS5-BC250-2026.11.10-beta1-x86_64.AppImage
```

The portable `.tar.gz`/ZIP contains the same program. Extract it, enter its directory,
and use `./AppRun` with the same arguments. If a ZIP extractor drops executable
permissions, run `chmod +x AppRun bin/* runtime/run-elf runtime-selector.sh selector-relocated.sh emulator/kyty_*`.
On a host without FUSE, extract the AppImage with `--appimage-extract` and run
`./squashfs-root/AppRun`, or use the tar archive.

Add your game folders/archives in the launcher. No games, console firmware, saves,
personal library, cached artwork or generated proprietary networks are included.
The beta starts with Upstream translation and frame generation Off.

## CachyOS, Bazzite and SteamOS

All packages target x86-64 AMD BC-250 Linux with **glibc 2.38 or newer**, a working
amdgpu/Mesa stack, X11/Xwayland, DRM render-node access and `/dev/kfd` access.
The package includes official .NET, portable Python, real SDL2, objdump, C++ runtime,
and the validated Proton/Wine 11.0 runtime for the optional HLX helper. No .NET,
Python, pip or development tools need installing on the tester's host.

The system GPU helper installs under `/var/lib/kyty-beta-system`; its service lives
under `/etc/systemd/system`. It does not need an unlocked read-only root or a layered
RPM. Recheck `--doctor` after OS updates. Bazzite and SteamOS physical gameplay
validation is still needed.

For existing Steam Gaming Mode, install the desktop shortcut and add **KytyPS5 Beta**
as a non-Steam application. Keep Steam's Proton compatibility override **disabled**
for this Linux application. This path uses the distro's existing gaming session.

Native GPU prerequisites from the emulator: for large games use approximately
512 MiB reserved VRAM and a 14 GiB GTT limit (`ttm.pages_limit=3670016`). The
upstream-recommended amdgpu settings are `sched_policy=2 vm_update_mode=0 cwsr_enable=0`.
`--doctor` reports current values. This installer does not edit BIOS, kernel arguments,
drivers, governor, clocks, initramfs, or boot entries.

## Helix and Lossless Scaling

Native Game upscaling and native FSR3 frame generation do not require a Helix import.
HLX FSR4 and Helix DLSS Model E use your own **locally prepared HelixSR 3.2.0** release.
Obtain the complete release and run its official setup on your own machine, then:

```sh
./AppRun --import-upscalers --helix-release /path/to/HelixSR-3.2.0
```

The official setup builds the required vendor network/shader files locally. The
Helix license prohibits redistributing those generated files; they are deliberately
absent here. See https://github.com/lonewolf0622/HelixSR/releases . The beta loads the
unmodified DLLs through the existing helper and bundled validated Wine 11.0.

Yotei Adaptive 60 FPS with MAKO 4.6.0 additionally needs your own Lossless Scaling
3.2.2 DLL and MAKO renderer:

```sh
./AppRun --import-upscalers --lossless-dll /path/to/your/Lossless.dll --mako /path/to/MAKO-Renderer-4.6.0
```

Fixed LSFG and the experimental adaptive LSFG route remain available for users
with those separate runtimes already installed. Selecting an unavailable optional
runtime prints a setup error instead of silently substituting another backend.

Astro's manual upscaling presets are rebound to the tester's game location only
after its title, version and executable SHA-256 match the validated build. Other
regions/versions retain their ordinary automatic/native route.

## Cheats and recovery

Cheat management and exact-version validation are included. The existing Yotei
02.002.000 and Demon's Souls 01.007.000 cheat plans are included with every cheat
disabled initially. Matching game versions discover their writable copy in Game
settings → Cheats. You can also import/select your own patch JSON. Personal enabled
selections and path-based profiles are not copied from the developer's machine. Demon's Souls
Scream assertion recovery remains an opt-in per-game setting; it is a narrow
workaround, and the latest backend integration has not had a new gameplay retest.

## Optional standalone minimal gaming mode

This is the low-background labwc mode, separate from Steam Gaming Mode. It is
experimental on other installations and requires the **host** to provide labwc,
Xwayland, xdotool, dbus-run-session, PipeWire/Pulse, WirePlumber, polkit and systemd
256 or newer. The installer checks these before changing files. It uses the host's
compositor/audio stack instead of installing a new desktop stack on an immutable OS.

```sh
./AppRun --install-gaming-mode
pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control check
pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control enter
```

Installing does not enter the mode. **Save work before entering:** entry ends the
configured user's existing sessions/apps and temporarily stops background services
including networking. Return through Control centre → Return to Desktop Mode, or
Super+Shift+E. A recovery login remains on Ctrl+Alt+F4:

```sh
sudo /var/lib/kyty-beta-system/session/ps5-minimal-control desktop
```

On systems with Valve's `steam-set-session`, return requests KDE. On other hosts
it restores the configured display-manager session/login screen; it does not
rewrite Bazzite's session manager configuration. Full DRM/VT handoff and physical
controller verification must be completed per distro before treating this as stable.
Use the existing Steam Gaming Mode path if the standalone mode's dependencies are
absent. KDE Bazzite/CachyOS/SteamOS are the intended initial standalone-session targets.

## Data, reports and removal

Beta data is separate from the developer's/older install:

* Settings: `~/.config/KytyPS5-Beta`
* Saves: `~/.local/share/KytyPS5-Beta/runtime/_SaveData`
* Shader caches/trophies: adjacent directories under `runtime`
* Upscaler runtime/logs: `~/.local/share/KytyPS5-Beta/upscalers`

`XDG_CONFIG_HOME`/`XDG_DATA_HOME` are respected; explicit `KYTYPS5_CONFIG_ROOT`,
`KYTYPS5_PROFILE_ROOT`, and `KYTYPS5_DATA_ROOT` overrides are supported.
Extracting/updating a package does not overwrite these directories.

`./AppRun --verify` runs isolated launcher/controller checks without launching a
game. `./AppRun --collect-report` saves a local report for you to review/share.
Reports contain system details and bounded log excerpts; no automatic uploads occur.
Report distro/version, kernel, game title/version, renderer/upscaler/FG choices,
reproduction steps and whether it happens with frame generation Off.

`./AppRun --uninstall-system` removes installed beta system files and restores
backed-up service definitions. Saves/settings are retained. Remove the desktop
shortcut with `./AppRun --install-desktop --remove`, then delete the package.

The accompanying source archive contains the launcher/package sources and frozen
emulator source provenance. Licenses and runtime notices are under `licenses/`.
