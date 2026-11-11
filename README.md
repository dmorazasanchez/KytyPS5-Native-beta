# KytyPS5-Native BC-250 Beta — a fork of Superkeka

This is an experimental, community-maintained **GitHub fork of [Superkeka’s KytyPS5-Native](https://github.com/superkeka/KytyPS5-Native)**, not a separate emulator or an official Sony project. Superkeka and the original KytyPS5 contributors created the emulator foundation. This BC-250 Linux beta adds a controller-first launcher, packaging, per-game upscaling and frame-generation integration, extracted-game and `.zar` support, and an optional standalone PS5 Minimal session. The optional HelixSR technology is credited to [Lonewolf](https://github.com/lonewolf0622/HelixSR).

**Beta 3** includes the newest SteamOS Minimal Mode PAM/EGL installer fixes and native GPU configuration warnings. Download the current prerelease from [Releases](https://github.com/dmorazasanchez/KytyPS5-Native-beta/releases). **Beta 2 binaries do not contain those fixes.** SteamOS and Bazzite remain experimental, with game compatibility strongly dependent on the kernel/AMDGPU setup.

The Beta 2 glibc 2.39 compatibility fix and bundled labwc/xdotool remain included. Beta 3 additionally preserves systemd-userdbd during PS5 Minimal Mode PAM startup and uses the host Wayland client library with host Mesa/EGL when running the bundled labwc.

[First run](#first-run) · [Upscaling and frame generation](#upscaling-and-frame-generation) · [PS5 Minimal Mode](#ps5-minimal-mode) · [SteamOS quirks](#steamos-quirks-and-limitations) · [Steam Gaming Mode](#steam-gaming-mode) · [Troubleshooting](#troubleshooting)

## Original projects and credits

> **This is a community fork, not an independent reimplementation.**
>
> - **[Superkeka — KytyPS5-Native](https://github.com/superkeka/KytyPS5-Native)** is the **main upstream project** and foundation of this BC-250 PS5 emulator. The native emulator, its core emulation work and original improvements belong to Superkeka and the upstream contributors. This repository builds on that work.
> - **[Lonewolf — HelixSR](https://github.com/lonewolf0622/HelixSR)** is the creator of **HelixSR**, whose upscaling work underpins the optional HLX/FSR 4 and HLX/DLSS integration used here. This fork does **not** claim authorship of HelixSR, and proprietary or separately licensed runtime assets must be obtained from their respective sources.
>
> Thanks also to the original [KytyPS5 project](https://github.com/KytyPS5/KytyPS5) and its contributors. This fork's packaging, launcher integration and BC-250-specific experiments are downstream additions; please credit the original authors when discussing or redistributing the project.

## System requirements

- An x86-64 AMD BC-250 system with glibc 2.38 or newer, a working amdgpu/Mesa stack, X11 or Xwayland, and user access to the DRM render node and `/dev/kfd`.
- For larger games, the upstream baseline is 512 MiB of reserved VRAM and a 14 GiB GTT limit (`ttm.pages_limit=3670016`). The recommended amdgpu settings are `sched_policy=2 vm_update_mode=0 cwsr_enable=0`. Run `--doctor` to see your current values. [Upstream GPU configuration](tools/bc250-amdgpu/README.md)
- The package bundles the launcher runtimes and Wine 11.0 for the optional Helix helper. PS5 Minimal Mode has its own host dependencies listed below.

## First run

Download the AppImage in **Desktop Mode** and keep it in the folder you plan to launch it from. Open a terminal in that folder and run the following commands as your normal user. The GPU helper installer asks for sudo when it needs it.

```sh
chmod +x KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --install-helper
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --doctor
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --install-desktop
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage
```

**The GPU helper and driver options are separate setup steps.** `--install-helper` installs `kyty-lodhw`; it does not apply `sched_policy=2 vm_update_mode=0 cwsr_enable=0` or change boot configuration. With the stock driver, `sched_policy=0` disables Kyty's default KFD queue path. The fallback can be slow or hang. Review `--doctor` warnings and the [GPU configuration instructions](tools/bc250-amdgpu/README.md); driver options take effect after a reboot. Changing them should be a deliberate host setup step.

`--install-desktop` adds **KytyPS5 Beta** to the application menu. The GPU helper runs as `kyty-lodhw.service` and uses `/var/lib/kyty-beta-system` and `/etc/systemd/system`. Installation works with the read-only system root on SteamOS and Bazzite.

For the ZIP or tar.gz, extract the package, open a terminal in the extracted folder, and use `./AppRun` in place of the AppImage filename in every command below. If the AppImage reports a FUSE error, use the tar.gz or run the AppImage with `--appimage-extract` and use `./squashfs-root/AppRun`.

Open **Add games** and select the folder containing your extracted games or `.zar` archives. An extracted game folder contains `eboot.bin`. Use your own game files.

**Updating from Beta 1 or 2.** Point any Steam shortcut at the Beta 3 AppImage. Run `--install-desktop` again to update the application menu shortcut. If you use PS5 Minimal Mode, rerun `--install-gaming-mode` from Desktop Mode to point the session at Beta 3.

## Upscaling and frame generation

Select a game in the library and open **Game settings → Upscaling**. Fresh beta profiles use the **Upstream** renderer, **Game default** upscaling, and frame generation **Off**. Keep the renderer on **Upstream** for the upscaler routes below. The launcher scans each game for a supported interface and shows the result on this settings page.

| Upscaler | What it uses | Setup |
| --- | --- | --- |
| **Game default** | The game's own reconstruction, such as TAA or FSR where the title provides it | Bundled |
| **Automatic** | HLX / FSR 4 on compatible titles after Kyty verifies the game's inputs. Other titles keep their own reconstruction | Import HelixSR before using this on an HLX-compatible title |
| **HLX / FSR 4** | The Helix FSR 4 path on supported titles | Import HelixSR |
| **HLX / DLSS (Model E)** | The Helix DLSS Model E path on supported titles | Import HelixSR |

Use **Render resolution preset** to adjust the internal resolution on a supported route. **Native / DLAA** renders at output resolution. **Quality**, **Balanced**, and **Performance** lower the internal resolution in that order. **Upscaled output resolution** sets the target separately. On the 4K HLX path, the 1800p and 1440p options reduce the reconstruction target and scale the result to the game's 4K output. Astro Bot's manual presets require the matching title ID, version, and executable hash.

Set **Native frame generation** on the same page. **FSR 3 · 2×** inserts a frame on supported games. **Automatic** enables it below 50 real FPS and the runtime can pause generation when shared memory is low. The matching manual Astro Bot profile uses fixed 2× when frame generation is enabled. You can adjust native frame generation during play under **PS button → Upscaling**. Set the upscaler and resolution preset before launching, since switching upscalers during play can crash the BC-250 when memory is tight.

### Install HelixSR for HLX

1. Download and extract `HelixSR-3.2.0.zip`, then run `./helixsr-setup.sh` from its extracted folder and follow the vendor's Linux setup prompts. The setup prepares the files needed for the Model E route. [HelixSR 3.2.0 release and setup instructions](https://github.com/lonewolf0622/HelixSR/releases/tag/v3.2.0)
2. Return to the folder containing KytyPS5 and import the prepared HelixSR folder. Replace the example path with the directory containing `HLX-FSR4/` and `HelixSR/`.

```sh
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --import-upscalers \
  --helix-release "/path/to/HelixSR-3.2.0"
```

3. Choose **HLX / FSR 4**, **HLX / DLSS (Model E)**, or **Automatic** in the game's Upscaling settings and launch the game. You can use **Game default** and native FSR 3 frame generation while you set up Helix.

### Optional Lossless Scaling frame generation

Open **Game settings → Frame generation** for the separate Lossless Scaling modes. Kyty switches native FSR 3 frame generation off for a launch using Lossless Scaling.

For **Ghost of Yōtei (PPSA26344)** in Upstream mode, **Adaptive 60 FPS** uses **MAKO Renderer 4.6.0** and your own **Lossless Scaling 3.2.2** `Lossless.dll`. Extract `MAKO-Renderer-v4.6.0-linux.tar.xz` and point the import at the folder containing `lib/libmako-render.so`. [MAKO Renderer 4.6.0 download](https://github.com/eugeniosegala/MAKO/releases/tag/render-v4.6.0)

```sh
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --import-upscalers \
  --lossless-dll "/path/to/your/Lossless.dll" \
  --mako "/path/to/extracted/MAKO-Renderer"
```

Select **Adaptive 60 FPS** and relaunch the game. **Fixed 2×** and Adaptive on other titles use separate LSFG Vulkan layers and profiles from an existing LSFG setup. The launcher reports the missing file or layer when a selected mode is unavailable.

<details>
<summary>Paths for existing LSFG setups</summary>

| Mode | Config file | Vulkan layer manifest | Profile name |
| --- | --- | --- | --- |
| Fixed 2× | `~/.config/lsfg-vk/kyty-bc250-2x-verified.toml` | `~/.local/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_frame_generation.json` | `Kyty BC-250 verified 2x` |
| Adaptive on the experimental LSFG route | `~/.config/lsfg-vk/kyty-bc250-adaptive.toml` | `~/.local/opt/lsfg-vk-experimental/share/vulkan/implicit_layer.d/VkLayer_LSFGVK_experimental_frame_generation.json` | `Kyty BC-250 Adaptive 60 FPS` |

</details>

## PS5 Minimal Mode

PS5 Minimal Mode starts Kyty in a dedicated labwc session with fewer background services. It is optional. The installer uses the host's labwc when it supports `--session` and its xdotool when available. Beta 2 includes labwc 0.8.3 and xdotool fallbacks under `/var/lib/kyty-beta-system/session-runtime`. The host provides **Xwayland, dbus-run-session, PipeWire, PipeWire Pulse, WirePlumber, polkit, PAM, and systemd 256 or newer**. The installer checks the dependencies before writing system files. If they are unavailable, you can run the beta from Desktop Mode or [Steam Gaming Mode](#steam-gaming-mode).

After completing the first-run setup, run the installer from **Desktop Mode**.

```sh
./KytyPS5-BC250-2026.11.11-beta3-x86_64.AppImage --install-gaming-mode
```

For an extracted archive, run `./AppRun --install-gaming-mode`. The installer adds **KytyPS5 Beta Minimal Mode** to the application menu. You can check the installed session before entering it.

```sh
pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control check
```

**Save your work before switching.** Entering Minimal Mode closes the configured user's current applications and sessions and temporarily stops background services, including networking. Open **KytyPS5 Beta Minimal Mode** from the application menu or run the following command from a local desktop.

```sh
pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control enter
```

To leave, open **Control centre → Return to Desktop Mode** or press **Super+Shift+E**. On hosts with Valve's session switcher, this requests KDE. Other hosts return to the configured display-manager session or login screen. If the session is stuck, press **Ctrl+Alt+F4**, log in, and run this recovery command.

```sh
sudo /var/lib/kyty-beta-system/session/ps5-minimal-control desktop
```

## Steam Gaming Mode

In Desktop Mode, run the first-run steps through `--install-desktop`. Add **KytyPS5 Beta** to Steam as a non-Steam game, leave **Force the use of a specific Steam Play compatibility tool** unchecked for this Linux launcher, then open it in Steam Gaming Mode.

## SteamOS quirks and limitations

**Minimal Mode launching successfully does not establish game compatibility.** The SteamOS observations below are from the BC-250 test system; results can differ across kernels, BIOS settings and games.

- **Immutable OS:** GPU helper and Minimal Mode installation writes only to /var/lib/kyty-beta-system and /etc, not /usr. The beta does not unlock SteamOS's read-only root, install kernel patches, configure AMDGPU boot options, or modify GRUB/Limine. Run the doctor after OS updates and after installing the helper.
- **Gaming Mode versus Minimal Mode:** Add the native Linux launcher to Steam as a non-Steam game, with Steam Play/Proton forced compatibility **disabled**. PS5 Minimal Mode is a separate optional labwc session; it is not the Steam client or Valve Gaming Mode.
- **PAM startup issue:** On SteamOS, userdbd was stopped during target isolation, causing ps5-minimal-session.service to fail with 224/PAM. Beta 3's system installer adds a conditional systemd service drop-in requiring and ordering systemd-userdbd when the host provides that unit. Running Beta 2's installer again will not install this fix.
- **EGL renderer issue:** The older bundled Wayland client lacked a symbol needed by SteamOS Mesa. Beta 3's installer builds a compatibility symlink to the host libwayland-client.so.0 and gives it priority only in the bundled labwc wrapper. It does not replace Mesa or change the library search path for Kyty or Xwayland. If host labwc supports the required session option, the installer uses the host version.
- **AMDGPU scheduler/VM/CWSR:** The helper installer and GPU driver setup are separate operations. SteamOS logs showed sched_policy=0, vm_update_mode=-1, cwsr_enable=1; that configuration disables Kyty's preferred KFD path. Run the doctor and consult [BC-250 GPU settings](tools/bc250-amdgpu/README.md). Stock and patched AMDGPU kernels require different settings; changing options takes effect on reboot.
- **VRAM and GTT:** For large titles, follow the upstream guidance to use 512 MiB UMA/VRAM and up to 14 GiB GTT, and ensure the user can open the render node and /dev/kfd. These are requirements to check, not changes made by the installer.
- **External library drives:** Keep the game-storage filesystem mounted. SteamOS paths such as /run/media/deck, /mnt and /var/mnt can differ after a reboot or repartition. Update stored library folders, LastGame entries and per-game profiles if a path moved; old saved locations are not evidence of an emulator backend defect.
- **Ghost of Yotei (PPSA26344):** The content-registration fix and the NV12 VideoDec2 output allocation correction were verified inside the earlier Beta 2 AppImage. Later SteamOS logs still reported a native COND_EXEC (opcode 0x22) translation refusal in frame 4, followed by queue waits. Disabling DCB chunking in a controlled diagnostic bypassed the immediate refusal but **did not demonstrate a complete fix**. Never skip this command or force queues signaled to hide the failure. Vulkan fragmentShaderBarycentric support was also missing in some SteamOS logs, but its role in this specific failure is unproven.
- **Logs and reproduction:** Capture the complete launch log, kernel version, AMDGPU options, game title/version, selected renderer, upscaler and FG profile. Test with frame generation disabled when comparing failure modes. Do not upload games, game keys, save files or private logs.

For evidence and exact limits of the testing, see [SteamOS PAM/EGL investigation](docs/steamos-bc250-minimal-mode.md) and [SteamOS AppImage/game path audit](docs/steamos-appimage-path-audit.md). These reports describe Beta 2 test observations, not confirmation of Beta 3 gameplay compatibility.

## Cheats and recovery

**Game settings → Cheats** shows matching patches for Ghost of Yōtei **02.002.000** and Demon's Souls **01.007.000**. Cheats start disabled and require the matching title ID and version. The Demon's Souls Scream assertion recovery is a separate opt-in per-game workaround.

## Troubleshooting

- If Steam shows only `ELFCLASS32` overlay warnings, run the Beta 2 AppImage in a Desktop Mode terminal and capture the full output. The overlay warnings alone give no backend failure.
- Run the AppImage with `--doctor` to check the BC-250, render node, KFD, and GPU helper. If the helper is missing, rerun `--install-helper` and check `systemctl status kyty-lodhw.service`.
- For an HLX launch error, confirm the Helix import completed or select **Game default**. For a Lossless Scaling error, set **Frame generation → Off** and retry.
- Run with `--verify` for launcher and controller checks. Run with `--collect-report` to save diagnostics locally. Include your distro and version, kernel, game and version, upscaler and frame generation selections, and steps to reproduce.

All ten Beta 2 backend routes reached `--help` on glibc 2.39, and the launcher passed **113 checks** there. Beta 3 retains those backend binaries and changes the system installer and GPU doctor. A headless test opened and focused the Kyty window using bundled labwc, Xwayland, and bundled xdotool. Gameplay on Bazzite and SteamOS still needs reproducible title-by-title validation. SteamOS Minimal Mode compositor launched after the observed local PAM/EGL fixes, but a fresh Beta 3 installation and physical game performance remain to be validated. [Beta validation reports](https://github.com/dmorazasanchez/KytyPS5-Native-beta/releases/tag/2026.11.11-beta3)

## Data and removal

| Data | Default location |
| --- | --- |
| Settings | `~/.config/KytyPS5-Beta` |
| Saves | `~/.local/share/KytyPS5-Beta/runtime/_SaveData` |
| Shader caches and trophies | Adjacent directories under `~/.local/share/KytyPS5-Beta/runtime` |
| Upscaler files and logs | `~/.local/share/KytyPS5-Beta/upscalers` |

The launcher respects `XDG_CONFIG_HOME` and `XDG_DATA_HOME`. You can also set `KYTYPS5_CONFIG_ROOT`, `KYTYPS5_PROFILE_ROOT`, and `KYTYPS5_DATA_ROOT` explicitly.

After returning to Desktop Mode, run the AppImage with `--uninstall-system` to remove the helper and minimal-session system files. Run with `--install-desktop --remove` to remove the shortcuts. The uninstall keeps beta saves and settings.

## Source and credits

**Primary acknowledgments:** [Superkeka / KytyPS5-Native](https://github.com/superkeka/KytyPS5-Native) (the core upstream fork and native GPU work) and [Lonewolf / HelixSR](https://github.com/lonewolf0622/HelixSR) (the optional HelixSR upscaler technology), plus the upstream KytyPS5 and all contributors. The optional HLX/FSR 4 and HLX/DLSS routes are integrations of Lonewolf's work, not original upscalers created by this fork.

The original project and Superkeka are credited under their respective licenses. Public source excludes proprietary NVIDIA NGX SDK headers, Helix-generated models/shaders, Lossless Scaling DLLs, console firmware and games. NVIDIA NGX headers must be obtained under a separate vendor license if rebuilding the optional helper. The release includes launcher/packaging source and third-party notices. [Beta source notes](beta/SOURCE.md) · [GPL-2.0 license](LICENSE) · [Original Kyty MIT notice](LICENSES/Kyty-MIT.txt)

<details>
<summary>Original upstream README and build instructions</summary>

# KytyPS5-Native

PS5 games natively on the PS5 chip under Linux.

A fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5) that runs PS5 games GPU work natively on the
AMD BC-250. The BC-250 has the PS5's APU ("Oberon", GPU gfx1013), so the title's command buffers
run on the hardware after a light translation, and its shaders run with small patches. Vulkan stays for
presentation and as a fallback. It will likely run on a PS5 under Linux too.

> [!IMPORTANT]
> Not affiliated with Sony Interactive Entertainment or PlayStation. No games or system software are
> distributed. Use only game files you obtained legally.

## Status

<table align="center">
  <tr>
    <td align="center">
      <strong>Gran Turismo 7</strong> (60 fps in races)<br>
      <img src="docs/screenshots/bc250-gran-turismo-7.jpg" width="300" alt="Gran Turismo 7 running natively on the BC-250">
    </td>
    <td align="center">
      <strong>ASTRO BOT</strong> (levels playable)<br>
      <img src="docs/screenshots/bc250-astro-bot.jpg" width="300" alt="ASTRO BOT running natively on the BC-250">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Stray</strong> (60 fps)<br>
      <img src="docs/screenshots/bc250-stray.jpg" width="300" alt="Stray running natively on the BC-250">
    </td>
    <td align="center">
      <strong>My First Gran Turismo</strong> (60 fps)<br>
      <img src="docs/screenshots/bc250-my-first-gran-turismo.jpg" width="300" alt="My First Gran Turismo running natively on the BC-250">
    </td>
  </tr>
  <tr>
    <td align="center">
      <strong>Dead Cells</strong> (60 fps)<br>
      <img src="docs/screenshots/bc250-dead-cells.jpg" width="300" alt="Dead Cells running natively on the BC-250">
    </td>
  </tr>
</table>

## Install

### SteamOS (recommended), Bazzite, CachyOS Handheld: Decky plugin

1. Install [Decky Loader](https://decky.xyz) and turn on its developer mode
2. Download the latest `KytyPS5-Native-*-Decky.zip` from [Releases](https://github.com/superkeka/KytyPS5-Native/releases)
3. Decky settings > Developer > Install Plugin from ZIP File
4. In the plugin: Kernel config > Apply and reboot, then Install, then pick the games folder

Games show up in the Steam library under Non-Steam as "Game (PS5)". The PS / Xbox button,
Share+Options together or F10 open the in-game menu (resume, monitoring, exit game). The plugin
keeps KytyPS5-Native and itself up to date.

### Other systems

Download the latest `KytyPS5-Native-*-Linux-x86_64.tar.gz` from
[Releases](https://github.com/superkeka/KytyPS5-Native/releases) and unpack it.

```bash
sudo ./install-helper.sh   # the root helper as a service (LOD statistics, KFD TLB flushes)
./kyty_launcher
```

On the BC-250 native mode turns on by itself; `KYTY_NATIVE_GPU=0` turns it off.
`kyty_emulator --game /path/to/game` runs a game without the launcher, `kyty_emulator --help` lists
the options.

## Requirements

- BC-250 with SteamOS (tested), stock or patched amdgpu
- Big games (Gran Turismo 7, Astro Bot): the modded P3.00 "Chipset Menu" BIOS with VRAM 512 MB
  (Chipset > GFX Configuration > UMA Frame Buffer Size). More VRAM takes the RAM games need.
  On the stock BIOS: `sudo ./bc250memcfg UMA_SIZE 512` with [bc250_memcfg](https://github.com/fanoush/bc250_memcfg), no flashing
- GTT 14 GiB (`ttm.pages_limit=3670016`). Without it games over 8 GiB of guest memory stall loading
- Recommended: `options amdgpu sched_policy=2 vm_update_mode=0 cwsr_enable=0`, or the patched amdgpu

The plugin sets the last two. Kernel modes and how to set them by hand: [`tools/bc250-amdgpu`](tools/bc250-amdgpu/README.md).

## Build

On SteamOS inside a [Distrobox](https://distrobox.it/) container with Clang, CMake, Ninja,
glslang and the SDL3 development libraries:

```bash
git submodule update --init --recursive
cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/linux-no-qt --target kyty_emulator --parallel
```

The helper `kyty-lodhw` is built with the emulator.

## Native backend

Code: [`src/graphics/native_gpu`](src/graphics/native_gpu).

- [Streamed DCBs](docs/native-gpu-streamed-dcbs.md): the GPU starts while the translation goes on
- [Debugging GPU hangs](docs/native-gpu-hang-debugging.md)
- [PS5 legacy GCN mode](docs/ps5-legacy-gcn-mode.md)
- [`tools/bc250-native-probe`](tools/bc250-native-probe/README.md): hardware probes
- [`tools/bc250-frame-diff`](tools/bc250-frame-diff/README.md): native and Vulkan frames compared

## License

[GNU General Public License version 2](LICENSE) (`GPL-2.0-only`). Based on
[Kyty](https://github.com/InoriRus/Kyty) (MIT, notice in [`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt)).
Third-party components keep their own licenses.

## Thanks

- [KytyPS5](https://github.com/KytyPS5/KytyPS5): the translation layer this fork builds on
- [InoriRus/Kyty](https://github.com/InoriRus/Kyty): the original Kyty
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4): PS4 memory, GPU aliasing, cache
  coherency and AVPlayer reference

</details>
