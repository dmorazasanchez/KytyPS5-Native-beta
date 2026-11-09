# Automatic upscaler and frame-generation integration

2026-11-09. Worktree `/home/david/kyty-auto-upscaler-20261109`, branch `experiment/auto-upscaler-20261109`
(seed `0631f62` = the native-upscaling tree's working state at 12:04, byte-identical `nativeUpscaler.cpp`).
Launcher work: `/home/david/kytyps5-auto-upscaler-launcher-20261109`, branch `experiment/auto-upscaler-launcher-20261109`.
Nothing installed, no game launched, no production file, game file or launcher file changed.

## What it does

```
game added / library refreshed            launch                                   first seconds of play
─────────────────────────────            ──────                                   ─────────────────────
KytyPS5 → kyty-upscaler-scan ─► profile  kytyps5-runtime.sh: profile says          Kyty: fingerprint in guest memory,
  (offline, cached by path+mtime,        "available"? → profile build +            60 invocations whose named bindings
   keyed by eboot SHA-256)               KYTY_UPSCALER_PROFILE, else production    match → HLX FSR 4 (+ FSR 3 FG if the
                                                                                    game runs < 50 FPS), result written
                                                                                    to <profile>.runtime.json
```

Per game nothing is tested by hand: the scanner derives every game-specific address from the executable, the
emulator re-checks the live bindings before it replaces anything, and a failure leaves the game's own upscaler.
The work that remains per *interface* (not per game) is an adapter, when a game uses an interface no adapter covers.

## Interfaces and adapters

| Interface | How it is identified (no per-game addresses) | Runtime hook | In this build |
|---|---|---|---|
| FidelityFX FSR 3.1 upscaler, PS5 AGC backend (`ffx_agc_fsr3upscaler_v1`) | Shader records through RELATIVE relocations of the SDK's pass names; job executor by the SDK job layout (stride 0x9a38, handle/record offsets); AgcCbDispatch return addresses incl. inline tail-jump thunks; executor job/backend stack slots by use pattern; FSR3 dispatcher by the SDK's UTF-16 diagnostics + description layout; its description slot (store of RSI); executor call through the backend interface table (fpExecuteGpuJobs follows fpScheduleGpuJob) | CaptureHost at AgcCbDispatch | **Implemented** (HLX FSR 4 / DLSS, FSR 3 FG) |
| PSML legacy r2 (`94iBp3KvIuI`, Demon's Souls 1.007 layout) | Import NID → PLT → call chain through wrappers by dataflow on the parameter register → parameter block (global or stack) → its stores checked against the revision's layout | Kyty's own HLE import (no game address needed) | Identified only (needs PS5 Pro mode + PSML→HLX boundary) |
| PSML legacy r1 (`RUNLFro+qok`, ASTRO BOT / DeS 1.005 layout) | same | same | Identified only |
| PSML MFSR2 (`lrJwpLjKXRc`) | same; layout from AnyPS5 (reverse-engineered) | same | Identified only |
| FidelityFX FSR2 / other FSR3 components | Pass-name clusters | — | Reported as candidate, no adapter |
| Custom TAA/TSR | — | — | Unsupported: the game keeps its image path |

Evidence ladder: `candidate` → `static_partial` → `static_layout_verified` (scanner) → `runtime_bindings_verified`
(Kyty, `<profile>.runtime.json`). Only an implemented adapter at `static_layout_verified` is enabled, and only after
the runtime check passes in that session.

## Results on the installed games (offline, scanner 1.0.0)

| Game | Routes found | Verdict |
|---|---|---|
| Ghost of Yōtei 02.002.000 | FSR3 AGC adapter **verified**; also PSML MFSR2 (Pro mode) verified against the AnyPS5 layout — a new result (game call `0xc2a091`, stack block `RBP-0xdb0`) | Upscaler + FG available; presets available (resolution controller known for this hash) |
| Demon's Souls 01.007.000 | PSML legacy r2 verified: wrapper `0x1e73ac0`, `SuperResolution::Apply` call `0xa53646`, global block `0x2f63f10`, all 13 of Codex's stores | Identified, not available (PS5 Pro route) |
| ASTRO BOT 01.018.000 (.zar) | PSML legacy r1 verified: game call `0x73cf56f`, stack block `RBP-0x180`, all 7 slot stores and 5 temporal stores of Codex's file | Identified, not available (PS5 Pro route) |

For Yōtei the scanner reproduces every constant the live-verified integration hard-coded (return PCs `0x5eef`/`0x5fcd`,
dispatcher return `0x16284`, slots −0xb0/−0xb8/−0x9b30, description store `0x15434`, shader headers
`0x1e84c40..0x1e85838`, record table `0x1c98880`). It also finds two more AgcCbDispatch calls in the executor
(`0x53a0`, `0x56c3`): those are its type-0/1 internal job paths, filtered by the existing job-type check.

## Tests

- `tools/upscaler-scan`: `python3 -I -m unittest discover -s tests` — 19 ground-truth tests against the three installed
  games (truth from the seed's source and Codex's identifier JSONs, not from the scanner) and 8 synthetic cases
  assembled with llvm-mc: global/stack blocks behind wrappers, another revision's layout rejected, clobbered or
  unresolvable parameter register, no callers. **27/27 pass.**
- `experiments/auto-upscaler/tests/profile_test.cpp` (built `-fno-exceptions` like Kyty): the emulator's parser reads
  the scanned Yōtei profile into exactly the built-in (previously hard-coded) values; DeS/Astro profiles are rejected
  with the scanner's reason; malformed JSON, wrong types, negative addresses, a positive frame slot are rejected
  without aborting; a profile without a resolution controller is usable with presets off. **Pass.**
- Emulator build `experiments/auto-upscaler/build/kyty_emulator_auto_upscaler` compiles and links (production flags,
  production objects read-only); the only warning is the seed's pre-existing `PoolProtected`.
- Launcher copy: `--verify` **106 checks pass** (3 new). Selector dry runs: Yōtei → profile build with
  `KYTY_UPSCALER=auto`, `KYTY_FRAMEGEN=auto`; DeS and ASTRO BOT → production build; scanner missing → previous
  Yōtei rule; Fast mode unchanged. A cached lookup takes 0.12 s, also for the 47 GB ASTRO BOT archive.

**Not yet done: a live run.** The profile build has not run a game. Until it has, it is not wired into the launcher.

## Emulator changes (`src/graphics/native_gpu/nativeUpscaler.cpp`, `upscaler/titleProfile.h`)

- All game-specific addresses come from the profile (`KYTY_UPSCALER_PROFILE`); without one, the built-in Yōtei
  profile equals the old constants.
- **Safety fix:** `ApplyLevel` wrote Yōtei's DRS override address every frame for whatever executable passed the
  checks; it now writes only when the profile maps the title's resolution controller.
- Runtime self-check (`NoteInvocation`): HLX and FG wait for 60 invocations whose bindings matched and whose inputs
  suit the HLX path; reasons for failures are kept and written to `<profile>.runtime.json`.
- `KYTY_UPSCALER=auto`: game FSR3 until verified, then HLX FSR 4 (`KYTY_UPSCALER_AUTO=hlx_dlss_e` for DLSS).
  `KYTY_FRAMEGEN=auto`: FSR 3 2× only if the title stays under 50 real FPS with its upscaler running (2× paces the
  title at half the refresh rate). Menu choices cancel the automatic ones.

## Installing (after a live run)

1. Launcher: `cd /home/david/sharpemu-upscaling-20261108 && git apply /home/david/kyty-auto-upscaler-20261109/integration/kytyps5-launcher-auto-upscaler.patch`,
   then `dotnet publish src/KytyPS5.Launcher/KytyPS5.Launcher.csproj -c Release --self-contained false -o artifacts/kytyps5`
   and `./run-kytyps5.sh --verify`. (Checked to apply cleanly to the tree as of 14:40; Codex was editing other launcher files.)
2. Profiles then appear in `~/.config/KytyPS5-Native/upscaler-profiles/` as the library loads.
3. Rollback: `git apply -R` the same patch; the old selector rule and validated build are untouched.

## Limits

- Pro-mode routes (DeS, ASTRO BOT, Yōtei's MFSR2) are identified and checked statically but not runnable: Kyty has to
  run the game's PS5 Pro path (ASTRO BOT and Yōtei fault before PSML dispatch in trinity mode, Stage 2 notes) and
  needs a PSML→HLX boundary. The design is known (chenxiao fork injects an `R_SUPER_RESOLUTION` packet from the HLE
  dispatch); the per-revision layouts are in `psml.py`.
- The FSR3 adapter expects the 0,4,5,6 pass sequence and reactive/composition masks; a game with RCAS sharpening,
  the shading-change passes or no masks is rejected at run time with that reason, game FSR3 kept.
- Render presets need a per-executable resolution-controller entry (`knowledge.py`); others run at the game's own
  dynamic resolution.
- Automatic HLX starts during the first seconds of FSR3 activity (usually the title screen). The findings' warning
  about switching upscalers in play when GTT is near its ceiling still applies; this switch happens early, when
  memory use is lowest.
