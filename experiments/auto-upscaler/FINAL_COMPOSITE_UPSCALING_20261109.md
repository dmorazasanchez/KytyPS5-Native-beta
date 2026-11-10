# Demon's Souls: final composite upscaling and frame generation

Published launcher executable: `build/kyty_emulator_launcher_taa_fg`.
SHA-256: `007f0ebae0b35afa5b1e8fa097a9470556b3421acc485a4c4693390ecc8e66ab`.
The user confirmed that this exact build now looks good. The successful run remains open.

## Image path

Ultra Quality renders at **2944x1656**. The game post-processes the render-sized scene; Helix reconstructs
that scene to **3840x2160** at the final scene/UI composite. The composite samples Helix's 4K texture
directly, with a private copy of its globals describing a 4K source. Its scene sampling is therefore 1:1;
it no longer spatially enlarges a render-sized TAA destination after Helix has run. The UI stays separate.

The exact-executable profile supplies the startup render constants and composite contract
`scene_ui_bicubic_v1`, shader hash `7246df104788f9d0`, scene path `[0,0,64]`.
Before that composite, the render-sized TAA shader is replaced with an exposure copy of its jittered
colour. Temporal reconstruction happens in Helix at the final composite. DLAA keeps the earlier TAA-boundary
route. Yotei keeps its existing FSR3/AGC binary route.

The runtime selector had bypassed preset setup for the separate TAA/FG binary. It now applies the selected
startup plan to that route as well. The settings/status report dimensions from the accepted Helix boundary,
rather than an intermediate TAA invocation. The selected user preset is Ultra Quality, output 4K, native FG 2x.

## Why FG still looked disabled after fixing resolution

Two corrections were needed after moving the upscaler:

1. Intermediate TAA jobs must not advance the displayed-frame sequence. Otherwise the new composite got
   every second boundary number, and the presenter reset temporal history on every frame. Intermediate
   exposure copies now have their own counter and descriptor-table slots; final composite IDs are consecutive.
2. The lower-resolution composite's explicit U transform cancels the vertex shader's X reversal. The
   displayed image is therefore in native render orientation. Applying the old DLAA mirror-X correction
   reversed the HUD-free image relative to the actual display. FSR's UI detector replaced interpolation
   with the real image. This composite uses **unmirrored** HUD-free colour, depth and motion samples,
   motion-vector scales **(-guide_width, -guide_height)**, and unreflected jitter. The original DLAA route
   still uses mirrored samples and **(+guide_width, -guide_height)**. Do not apply one route's transform
   to the other.

## Evidence

- `logs/fg-composite-proof/`: before the orientation correction, generated images were over 99.9% identical
  to the real image. Flipping the HUD-free image at full resolution aligned a central crop within one 10-bit
  code for 99.67% of pixels, proving the orientation error. Flip the full-resolution image before subsampling:
  flipping after subsampling adds a misleading three-pixel offset on a 4K image sampled every four pixels.
- `logs/fg-composite-aligned-proof/`: six saved real/generated/guide/HUD-free frame sets with matching
  requested and actual boundary IDs. The HUD-free central scene matches the real image within one code for
  **99.96-99.97%** of sampled pixels. Generated images differ from real images over **30-36%** of sampled
  pixels in these captures. Counters alone were not accepted as evidence.
- `fixes/20261109-composite-upscale/aligned-frame-metrics.json`: measurements from the retained raw captures.
- The user confirmed the corrected run looks good. The same executable was copied atomically to the main
  launcher's TAA/FG path, without restarting that successful run.
- The native build linked, `git diff --check` passed, scanner tests passed 27/27, and the FG guard regression
  test passed. The GPU-capacity requirement remains removed as requested; RAM/PSI checks remain.

Temporary frame-capture exports were removed from the selector before the successful diagnostic launch.
The normal launcher does not capture frames. Backups and the prior/test profile are in
`fixes/20261109-composite-upscale/`.

## Local memory measurements

The earlier RAM samples are retained in `fixes/20261109-fg-no-gpu-threshold/ram-samples.jsonl` and
`ram-report.json`. They were collected during the overloaded full-resolution test, before the final
render-path correction. Linux was swapping aggressively; resident memory alone understates the process
footprint. Values below are sample medians in MiB, with background-game samples filtered to require that
the game process was still running.

| Process/state | Resident RAM | Swap | PSS plus swap |
| --- | ---: | ---: | ---: |
| Local Codex processes, working before game launch | 240 | 113 | 352 |
| Local Codex processes, waiting with the game running | 33-58 | 214-231 | 275-279 |
| Launcher focused, game running | 5 | 92 | 96 |
| Launcher in background, game running | 3 | 92 | 95 |

The launcher used about 101 MiB resident (158 MiB PSS plus swap) before the game started.
Compilation temporarily peaked at an additional 471 MiB RSS for the compiler/linker process tree.
The Codex group covers the local daemon, terminal client and tool host; it does not measure remote
model-server memory. The launcher was subsequently closed by the user; the successful game run is left open.
