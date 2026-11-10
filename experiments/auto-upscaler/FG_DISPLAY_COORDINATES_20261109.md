# Demon's Souls frame generation: display coordinates

**Current integration:** see [FINAL_COMPOSITE_UPSCALING_20261109.md](FINAL_COMPOSITE_UPSCALING_20261109.md).
The lower-resolution final-composite route has a different orientation from the original DLAA route
described below. The published launcher hash is now
`007f0ebae0b35afa5b1e8fa097a9470556b3421acc485a4c4693390ecc8e66ab`.

The smooth-motion fix is installed for the main launcher in `build/kyty_emulator_launcher_taa_fg`.
Gameplay-validated coordinate-fix SHA-256: `7ceeff263e16b6ae52f7b1021c4fa2cb7b50755bde8cba00fead74e977056cf5`.
The current launcher build adds the startup-status correction and removes the FG GPU-capacity
requirement as described below, SHA-256
`36b51e038378435c3df7912cc6ca2d6837aa04612101d2a2d739d82c6600c860`.
The stock production emulator is unchanged. Before-change sources, profiles and executables are in
`fixes/20261109-hudless-orientation/`.

## Cause and correction

The final scene composite reverses X. The previous HUD-free capture did not: FSR therefore classified most of
the scene as UI and replaced interpolation with the current real frame. In one moving capture 99.3% of sampled
generated pixels were identical to the current real frame, despite the log counting 30 generated presents/s.

The depth and motion guides also need their sample positions mirrored. The earlier empirical conclusion that
the native vectors were `previous - current` conflated their native convention with this display reflection.
The native TAA convention is `current - previous`; native HLX scales are `(-width, -height)`. After reflecting X
for presentation, FG scales are `(+guide_width, -guide_height)`, and X jitter is reflected too. Do not restore a
single positive sign for both axes. Warping real moving frames with mirrored guide positions and these signs
gave background correlations 0.96–0.98, versus 0.62–0.75 for the uncorrected coordinates.

The discovered and installed profiles now contain `taa.display_mirror_x: true`. This defaults to false for
other titles, including the built-in Yotei profile. Both `fg_guides.s` and `fg_hudless.s` receive the flag through
their descriptor tables. Generated GPU machine code was rebuilt with `gen.sh`.

## Validation

- Scanner tests: 27/27 passed. The standalone parser check accepted the DeS transform and kept absent/invalid
  transforms and built-in Yotei unmirrored. `git diff --check` passed.
- `logs/fg-display-coordinates-215156` / `logs/capture-demons-sclwzU`: completed the 110-second timed run. No GPU
  fault. The user confirmed smooth motion, with minor artifacts.
- HUD-free central scene: 100% of sampled pixels match the real frame within one 10-bit code. The remaining
  full-frame differences are at the HUD.
- A real background shift of 40 pixels became two 20-pixel shifts through the generated frame; two other
  16-pixel shifts became 8 + 8. These are actual interpolated frames.
- `fixes/20261109-hudless-orientation/verify_capture.py` and `after.json` retain the measurements. The verifier
  keeps the latest metadata per filename because changing upscalers recreates the generator and restarts its
  dump filenames, overwriting earlier captures.
- The normal launcher selector was dry-run checked: it selects the installed experimental build, matching
  profile and `KYTY_FRAMEGEN=fsr3`.

Two follow-up launches encountered the existing startup gfx stall at tracker 295 before FG initialization;
the next retry with the same binary reached gameplay. This fix does not address that startup issue.

## Resolution experiments (separate from the published TAA/FG build)

The full-resolution Helix FSR 4 test (`logs/fg-helix-fsr4-215453`) received a 3840x2160 input and output (DLAA).
HLX ran successfully, while FG remained paused by its shared-memory guard. The user requested Ultra Quality.
The existing TAA route ignores preset selection because no render controller is mapped, so a lower label alone
would not change the actual input.

`fixes/20261109-hudless-orientation/demons-ultra-quality-2944x1656.json` is a reversible startup-only game patch
for the exact installed 1.007 executable. It changes the cinematic mode to load its 4K output pair separately
from the 2944x1656 render pair, reusing the performance mode's separate render load. It changes no file in the
game installation. Its first trial also changed render-target capacity tables and produced a 2944x1656 TAA
destination; those table changes were removed before the second trial. Live input/output validation is required
before calling this a working 4K Ultra Quality path. These experiments are not enabled by the main launcher's
stable TAA/FG route. Its discovered contract excludes `startup_render_presets`; that experimental block is saved
separately in `fixes/20261109-hudless-orientation/startup-render-presets-experimental.json`.

## Main launcher integration

The main launcher at `/home/david/sharpemu-upscaling-20261108/run-kytyps5.sh` now selects
`kyty_emulator_launcher_taa_fg` for a supported `taa_paths_v1` profile. It passes the selected `KYTY_UPSCALER`
and `KYTY_FRAMEGEN` values through to this exact validated build, including explicit FG Off. FSR3/AGC titles
such as Yotei keep their existing binary route. The user's game paths, saved choices and cheat-plan argument
remain in place.

Validation: the rebuilt launcher passed 113 checks, including the native-FG dashboard summary, new DeS Helix FSR4/DLSS plus FG and explicit Off
checks. Six real-launch-command selector checks verified the stable TAA route for Game TAA plus FG, either
Helix backend plus FG, Automatic FG and Off. The Game/Off case keeps the user's ordinary renderer/guard route.
`fixes/20261109-hudless-orientation/main-launcher-routing.json` records the results and Yotei's unchanged route.
The coordinate-fix build was used for the gameplay capture that the user confirmed as smooth.

## Frame-generation startup status correction

The launch at 23:02 showed successful Helix frames while FG retained its earlier
"resumes once the upscaler has started" message. The paused recovery branch only cleared
that message on a successful resume; it never replaced it with the current memory blocker.

The guard now re-evaluates and displays the wait reason on every check. It logs transitions
between helper startup, GPU capacity, RAM, memory pressure and the existing five-second
recovery window. A new helper start resets that window even if FG was already paused.
Helix readiness is published after the first successful frame completes. Memory thresholds
are unchanged; the HUD-less FG allocation estimate shown in the initial GPU wait is now
395 MB rather than the base 260 MB.

The installed build was launched through the main dashboard with the user's saved settings.
`logs/frontend-20261109-231003.jsonl` confirms `hlx_starting` followed by successful Helix
frames and `gpu_limit`. The on-screen status agrees: 13,893 of 14,336 MB GPU memory used,
FG needing 395 MB plus the existing 256 MB reserve. The input and output are still both 4K;
FG remains paused in this combination. The earlier full-resolution Helix test also paused FG.
The smooth-FG capture above validated Game TAA plus FG, not concurrent Helix plus FG.

`fixes/20261109-fg-startup-status/` contains the previous executable/source, regression test,
live telemetry, screenshot and verification record. The test covers startup-to-GPU/RAM/PSI
wait transitions and a complete uninterrupted recovery window. It passed, the native build
linked, and `git diff --check` passed.

## GPU-capacity threshold removed on request

The user explicitly requested removing the FG GPU-memory requirement. The Demon's Souls
launcher build no longer uses GPU capacity or the 256 MB GPU reserve to block initial
selection, pause active FG or delay recovery. GPU usage remains telemetry. The separate
RAM headroom, sustained memory-pressure, helper-startup and five-second recovery checks
remain. Helix's own startup admission check is separate from the FG policy.

`fixes/20261109-fg-no-gpu-threshold/` contains the previous build and sources, the new
guard regression test, build memory profile, and RAM sampler. The regression test passed;
the build linked successfully. The first live launch hit the existing tracker-295 graphics
stall before FG initialization and was retried without changing the watchdog.
