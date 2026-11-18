#!/usr/bin/env bash
# Shared native builds reserve and verify their graphics VMID before title GPU work.
# Enabled in the backend for every game; no per-title or experimental environment switch.
# All previous binary experiments remain available only through the Fast profile.
# The upstream path removes unrelated SharpEmu/BC-250 experiment overrides.
set -euo pipefail
bundle_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
beta_data="${KYTYPS5_DATA_ROOT:?}"

mode="${KYTY_NATIVE_GPU_PM4_MODE:-upstream}"
dry_run="${KYTY_EMULATOR_SELECTOR_DRY_RUN:-0}"
# Presentation-only choice from the SharpEmu frontend. Never applied to the
# native renderer itself; only the host Vulkan presentation layer.
fg_mode="${KYTY_SHARP_FG_MODE:-off}"
scream_requested="${KYTY_DEMONS_SCREAM_ASSERT_RECOVERY:-0}"
scream_title="${KYTY_BC250_TITLE_ID:-}"
user_spin_pacer="${KYTY_NATIVE_GPU_SPIN_PACER:-}"
audio_test="${KYTY_BC250_AUDIO_TEST:-off}"
audio_binary="$bundle_root/emulator/kyty_emulator_audio"
case "$fg_mode" in off|fixed|adaptive) ;; *) fg_mode=off ;; esac
unset KYTY_SHARP_FG_MODE
# Native upscaler build (copy of the selector for the upscaling frontend build): captured before the KYTY_* strip.
up_build="${KYTY_NATIVE_UPSCALER_BUILD:-0}"; up_kind="${KYTY_UPSCALER:-game}"; up_preset="${KYTY_UPSCALE_PRESET:-dynamic}"
up_output="${KYTY_UPSCALE_OUTPUT:-}"; case "$up_output" in 1800|1440) ;; *) up_output= ;; esac
up_fg="${KYTY_FRAMEGEN:-}"; case "$up_fg" in off|fsr3|fsr3_dynamic|auto) ;; *) up_fg= ;; esac
case "$up_kind" in auto|game|hlx_fsr4|hlx_dlss_e) ;; *) up_kind=game ;; esac
unset KYTY_NATIVE_UPSCALER_BUILD KYTY_UPSCALER KYTY_UPSCALE_PRESET KYTY_UPSCALE_OUTPUT KYTY_FRAMEGEN KYTY_RENDER_PRESET_PATCH
# The validated copy (development builds use other names, so they never change what the launcher runs).
upscaler_binary="$bundle_root/emulator/kyty_emulator_native_upscaler"
unset ENABLE_LSFGVK ENABLE_LSFGVK_EXPERIMENTAL LSFGVK_CONFIG LSFGVK_PROFILE LSFGVK_ENV
unset VK_ADD_IMPLICIT_LAYER_PATH
# Presentation settings are opt-in for each launch, including inherited MAKO settings.
while IFS= read -r name; do
    case "$name" in MAKO_*|ENABLE_MAKO*|DISABLE_MAKO*) unset "$name" ;; esac
done < <(compgen -e)
export DISABLE_MAKO=1 DISABLE_MAKO_SPATIAL_SCALING=1
export DISABLE_LSFGVK=1 DISABLE_LSFGVK_EXPERIMENTAL=1

upstream="$bundle_root/emulator/kyty_emulator_native"
upstream_data="$beta_data/runtime"
upstream_guard="$bundle_root/emulator/kyty_emulator_guard"
safe="$bundle_root/emulator/kyty_emulator_fast"
safe_guard="$bundle_root/emulator/kyty_emulator_fast_guard"

if [[ "$mode" == "upstream" ]]; then
    # The earlier selector silently fell back to an old Demon\'s Souls renderer.
    # NEVER make that substitution in this clean upstream profile.
    selected="$upstream"
    # Demon's Souls' Scream assertion recovery is part of the main backends now (loader/demonsScreamRecovery.h): it
    # runs on them with the newest fixes, armed by the variables below. The separate guard build stays only for the
    # dialogue audio test.
    if [[ "$scream_title" == "PPSA01341" && "$scream_requested" == "1" && "$audio_test" == "dialogue" ]]; then
        selected="$audio_binary"
    fi

    # The custom SharpEmu bridge currently exports KFD=0, partial GEM residency,
    # skip-shader controls, and other experiments. NONE are allowed to reach the
    # native backend. The game's native GPU path selects its own defaults.
    while IFS= read -r name; do
        case "$name" in
            KYTY_*|ENABLE_LSFGVK|LSFGVK_*) unset "$name" ;;
        esac
    done < <(compgen -e)
    if [[ "$scream_title" == "PPSA01341" && "$scream_requested" == "1" ]]; then
        export KYTY_DEMONS_SCREAM_ASSERT_RECOVERY=1 KYTY_BC250_TITLE_ID=PPSA01341
        if [[ "$selected" == "$audio_binary" ]]; then export KYTY_BC250_AUDIO_TEST=dialogue; fi
    fi
else
    # Preserve the existing rollback/experimental profile for separate tests.
    if [[ "${KYTY_BC250_TITLE_ID:-}" == "PPSA01341" &&
          "${KYTY_DEMONS_SCREAM_ASSERT_RECOVERY:-0}" == "1" ]]; then
        selected="$safe_guard"
    else
        selected="$safe"
    fi
fi

# Identify the game before choosing its presentation backend.
content_title="$scream_title"
if [[ "$mode" == upstream && -z "$content_title" ]]; then
    game_arg= prev=
    for a in "$@"; do
        if [[ "$prev" == --game ]]; then game_arg="$a"; fi
        prev="$a"
    done
    if [[ -n "$game_arg" ]]; then
        content_title="$(python3 -I "$bundle_root/prepare-runtime.py" --title "$game_arg")"
    fi
fi

# Marvel's Wolverine paces its frames with a compute shader that spins on the GPU clock until a target its CPU
# computed; on this path every frame then waited a full extra flip interval (15 fps Fidelity, 30-40 Performance).
# Without that dispatch the game runs at its GPU's pace with flips at the vblanks: Fidelity 30, Performance ~50.
# KYTY_NATIVE_GPU_SPIN_PACER=keep (set by the user) leaves it in.
if [[ "$mode" == upstream && "$content_title" == PPSA03671 ]]; then
    export KYTY_NATIVE_GPU_SPIN_PACER="${user_spin_pacer:-skip}"
fi

# Lossless Scaling frame generation through the bundled MAKO Renderer 4.6.0 Vulkan layer (GPL-3.0). The only
# file the user provides is Lossless.dll: found in a Steam install of Lossless Scaling, or imported once with
# --setup-lossless. The selected emulator ELF and its GPU command parameters are identical with LSFG on or off.
mako_layer="$bundle_root/framegen/mako/share/vulkan/implicit_layer.d"
if [[ "$fg_mode" != off ]]; then
    mako_config="$(python3 -I "$bundle_root/prepare-runtime.py" --mako-config)" || mako_config=
    if [[ -z "$mako_config" || ! -f "$mako_layer/VkLayer_MAKO_render.json" ]]; then
        echo '[Kyty launcher] Lossless Scaling frame generation is off: Lossless.dll was not found. Install Lossless Scaling from Steam, or run AppRun --setup-lossless /path/to/Lossless.dll.' >&2
        fg_mode=off
    else
        unset DISABLE_MAKO
        export ENABLE_MAKO=1 MAKO_CONFIG="$mako_config"
        case "$fg_mode" in
            fixed) export MAKO_PROFILE="Kyty Fixed 2x" ;;
            adaptive)
                # The validated Yotei profile (Quality/Performance 30 FPS base, 60 FPS output).
                if [[ "$mode" == upstream && "$content_title" == PPSA26344 ]]; then export MAKO_PROFILE="Yotei Dynamic 60 FPS"
                else export MAKO_PROFILE="Kyty Adaptive 60 FPS"; fi ;;
        esac
        export VK_ADD_IMPLICIT_LAYER_PATH="$mako_layer"
        # This single-GPU host crashes in Mesa's device-selection layer when MAKO creates an instance.
        export NODEVICE_SELECT=1
        export DISABLE_LSFGVK=1 DISABLE_LSFGVK_EXPERIMENTAL=1 DISABLE_MAKO_SPATIAL_SCALING=1
        export VK_LOADER_DEBUG=layer
    fi
fi
# One frame generator only: Lossless Scaling replaces Kyty's own FSR 3 frame generation for this launch.
if [[ "$fg_mode" != off ]]; then up_fg=off; export KYTY_FRAMEGEN=off; fi
# Only on LSFG launches: diagnose whether the Vulkan loader enables the layer (stderr goes to the game's log).
if [[ "$fg_mode" != off ]]; then
    printf '[KytyPS5 LSFG diagnostic] mode=%s profile=%s config=%s layer_dir=%s\n' \
        "$fg_mode" "${MAKO_PROFILE:-}" "${MAKO_CONFIG:-}" "${VK_ADD_IMPLICIT_LAYER_PATH:-default}" >&2
fi
# Yotei also needs license labels for add-ons it cannot mount. Keep the mounted data
# for any labels the selected DLC directory already supplies.
if [[ "$mode" == "upstream" && "$content_title" == "PPSA26344" ]]; then
    dlc_dir= prev=
    for arg in "$@"; do
        if [[ "$prev" == --addcont-dir ]]; then dlc_dir="$arg"; break; fi
        prev="$arg"
    done
    installed_labels=
    if [[ -d "$dlc_dir" ]]; then
        installed_labels="$("$upstream" --list-addon-labels "$dlc_dir" "$content_title")"
    fi
    for label in GHOST2APP0000000 GHOST2BASE000000 GHOST2BASEME0000 GHOST2CEUPGRADE0 \
        GHOST2COLLECTORS GHOST2DDE0000000 GHOST2DISC000000 GHOST2DLC0000000 GHOST2DLCPREPURC \
        GHOST2DLCREVIEW0 GHOST2LEGENDS000 GHOST2LEGENDSUPG GHOST2PREORDER00 GHOST2SAPPORO000 GHOST2SHIP000000; do
        if ! grep -Fxq "$label" <<< "$installed_labels"; then
            set -- "$@" --addcont "$label"
        fi
    done
fi

# Native upscaling for any game whose profile has a supported route: kyty-upscaler-scan identifies the game's
# upscaler interface offline when the launcher adds it (or here, once; cached by path and size/mtime). The profile
# build takes every game-specific address from that profile and replaces nothing before its runtime self-check has
# matched the live bindings. Games without a supported route keep the production build and their own upscaler.
auto_binary="$bundle_root/emulator/kyty_emulator_auto_upscaler"
taa_binary="$bundle_root/emulator/kyty_emulator_launcher_taa_fg"
scanner="${KYTYPS5_UPSCALER_SCANNER:-$bundle_root/tools/upscaler-scan/kyty-upscaler-scan}"
up_profile= up_state= up_fg_state= up_adapter=
# Manual Astro integration: exact installed archive and stamp; no scanner/discovery on this route.
astro_game_arg= prev=
for a in "$@"; do
    if [[ "$prev" == --game ]]; then astro_game_arg="$a"; fi
    prev="$a"
done
astro_root="$beta_data/astro/not-matched"
if [[ "$up_build" == 1 && "$mode" == upstream && "$content_title" == PPSA21567 ]]; then
    # A copy this package cannot bind (another region/version, an unreadable archive) is not an error: the game
    # then takes the scanned route below, or its own upscaler. Never refuse the launch over it.
    bound="$(python3 -I "$bundle_root/prepare-runtime.py" --astro "$astro_game_arg")" || {
        echo '[Kyty launcher] The tested ASTRO BOT profile could not be matched to this copy; continuing without it.' >&2
        bound=
    }
    if [[ -n "$bound" ]]; then astro_root="$bound"; fi
fi
astro_binary="$bundle_root/emulator/kyty_emulator_astro"
astro_manual=0
if [[ "$up_build" == "1" && "$mode" == upstream && -f "$astro_root/astro-manual.json" ]]; then
    astro_game_arg= prev=
    for a in "$@"; do
        if [[ "$prev" == --game ]]; then astro_game_arg="$a"; fi
        prev="$a"
    done
    if [[ -f "$astro_root/astro-manual.json" ]]
    then
        [[ -x "$astro_binary" ]] || { echo 'Astro manual executable is missing' >&2; exit 2; }
        astro_manual=1
        up_profile="$astro_root/astro-manual.json"
        if [[ "$up_kind" != game ]]; then
            # Fixed render tiers (upscalers/astro-templates, make-astro-presets.py): Ultra Quality 2944x1656, Quality
            # 2560x1440, Balanced 2304x1296, Performance 1920x1080, Ultra Performance 1280x720, Native 3840x2160 (dlaa),
            # each upscaled to 4K. Dynamic keeps the game's own resolution tiers (2432x1368 output).
            case "$up_preset" in
                dlaa) astro_preset=native-dlaa ;;
                ultra_quality|quality|balanced|performance|ultra_performance) astro_preset="${up_preset//_/-}" ;;
                *) astro_preset= ;;
            esac
            if [[ -n "$astro_preset" && -f "$astro_root/astro-$astro_preset.json" && -f "$astro_root/astro-$astro_preset-render.json" ]]; then
                up_profile="$astro_root/astro-$astro_preset.json"
                export KYTY_RENDER_PRESET_PATCH="$astro_root/astro-$astro_preset-render.json"
            else
                if [[ "$up_preset" != dynamic ]]; then
                    printf '[Kyty launcher] ASTRO BOT has no %s render template; using the game'"'"'s own dynamic resolution.\n' "$up_preset" >&2
                fi
                up_preset=dynamic
            fi
        fi
        up_state=available up_fg_state=available up_adapter=taa_paths_v1
        # The user's Astro choice is fixed 2x. Its elapsed-time fix keeps simulation independent of real FPS.
        if [[ "$up_fg" != off && "$fg_mode" == off ]]; then up_fg=fsr3; fi
        export KYTY_ASTRO_DECOUPLE=1 KYTY_FRAMEGEN_WORKING_HEIGHT=720 KYTY_FRAMEGEN_OPTICAL_FLOW_DIVISOR=2
        export KYTY_FRAMEGEN_MV_CHECK=0 KYTY_LOD_STATS_MODE=banks
    fi
fi
if [[ "$astro_manual" == "0" && "$up_build" == "1" && "$mode" == "upstream" && -x "$auto_binary" && -f "$scanner" ]]; then
    game_arg= prev=
    for a in "$@"; do
        if [[ "$prev" == "--game" ]]; then game_arg="$a"; fi
        prev="$a"
    done
    scan_path="${game_arg%%!/*}" # an archived game is passed as <archive>.zar!/<game root>
    if [[ -n "$scan_path" ]]; then
        # Keep the scanner's own message: a failed scan must be visible in the game's log, never silent.
        scan_log="$(mktemp "${TMPDIR:-/tmp}/kyty-scan.XXXXXXXX" 2>/dev/null)" || scan_log=/dev/null
        up_profile="$(python3 -I "$scanner" ensure "$scan_path" 2>"$scan_log")" || up_profile=
        if [[ -z "$up_profile" ]]; then
            scan_why="$(tail -n 1 -- "$scan_log" 2>/dev/null)" || scan_why=
            echo "[Kyty launcher] The upscaler scan of this game failed (${scan_why:-no message}); it runs without a scanned upscaler profile." >&2
        fi
        if [[ "$scan_log" != /dev/null ]]; then rm -f -- "$scan_log"; fi
    fi
    if [[ -n "$up_profile" && -f "$up_profile" ]]; then
        read -r up_state up_fg_state up_adapter < <(python3 -I - "$up_profile" <<'PY'
import json, sys
try:
    p = json.load(open(sys.argv[1], encoding='utf-8'))
    c = p.get('capabilities', {})
    print(c.get('upscaler', {}).get('state', 'unknown'), c.get('frame_generation', {}).get('state', 'unknown'),
          (p.get('runtime') or {}).get('adapter', 'unknown'))
except (OSError, ValueError, AttributeError):
    print('unknown unknown unknown')
PY
) || true
    fi
fi
# Marvel's Wolverine: the static scan does not find its temporal upscaler (the resolve was found at run time), so the
# package carries the tested profile (upscalers/title-profiles) for the executable it was made with. It is used only
# when this copy's executable is that one (same SHA-256 as the scan's); a copy into the profile folder keeps the
# calibration the emulator caches next to it. The render presets fix the render size; Dynamic keeps the game's own.
title_profiles="$bundle_root/upscalers/title-profiles"
if [[ "$astro_manual" == "0" && "$up_build" == "1" && "$mode" == upstream && "$content_title" == PPSA03671 &&
      -n "$up_profile" && -f "$up_profile" ]]; then
    tested="$(python3 -I - "$up_profile" "$title_profiles" <<'PY'
import json, sys, pathlib
try:
    sha = json.load(open(sys.argv[1], encoding='utf-8'))['game']['sha256']
    for p in sorted(pathlib.Path(sys.argv[2]).glob('PPSA03671-*.json')):
        d = json.load(open(p, encoding='utf-8'))
        if isinstance(d, dict) and d.get('schema') == 'kyty-upscaler-profile' and d.get('game', {}).get('sha256') == sha:
            print(p)
            break
except (OSError, ValueError, KeyError, TypeError):
    pass
PY
)" || tested=
    if [[ -n "$tested" ]]; then
        tested_copy="$(dirname -- "$up_profile")/$(basename -- "$tested" .json).tested.json"
        # The tested calibration (jitter, motion vectors) as the first cache: the first launch starts calibrated.
        tested_cal="${tested%.json}.calibration.json"
        if [[ -f "$tested_cal" && ! -f "${tested_copy%.json}.calibration.json" ]]; then
            cp -f -- "$tested_cal" "${tested_copy%.json}.calibration.json" 2>/dev/null || true
        fi
        if cp -f -- "$tested" "$tested_copy" 2>/dev/null; then
            up_profile="$tested_copy" up_state=available up_fg_state=available up_adapter=taa_static_v1
            # HLX upscales to the game's own target (4K / 1440p, see below); a smaller HLX output would only be scaled
            # back up into it. The output choice still sets the window (the presenter scales to the display).
            if [[ -n "$up_output" ]]; then
                # The launcher requests a 4K window for intermediate HLX output sizes. This title
                # uses its own internal output size, so apply the chosen size to its window here.
                case "$up_output" in
                    1440) window_width=2560 ;;
                    1800) window_width=3200 ;;
                esac
                window_args=()
                while (($#)); do
                    case "$1" in
                        --screen-width|--screen-height) shift; (($#)) && shift ;;
                        --screen-width=*|--screen-height=*) shift ;;
                        *) window_args+=("$1"); shift ;;
                    esac
                done
                set -- "${window_args[@]}" --screen-width "$window_width" --screen-height "$up_output"
            fi
            up_output=
            # Render presets (wolverine-render-preset.py): each graphics mode renders at its own target / the preset's
            # ratio (Fidelity and the menus upscale to 3840x2160, Performance to 2560x1440, whatever the window size;
            # the output choice only scales the window), always below the target (Native AA one step below).
            # Dynamic keeps the game's own resolution range.
            # With frame generation on, Fidelity renders at most 1920x1080: larger sizes leave its 30 fps no room for
            # frame generation and fall to 20-28 (measured; see the script).
            wolverine_drs= wolverine_fg= wolverine_fg_note=
            case "$up_fg" in fsr3|fsr3_dynamic|auto) wolverine_fg=--framegen ;; esac
            [[ "$fg_mode" != off ]] && wolverine_fg=--framegen
            case "$up_preset" in
                dlaa|ultra_quality|quality|balanced|performance|ultra_performance)
                    wolverine_drs="$(dirname -- "$up_profile")/PPSA03671-render-${up_preset}${wolverine_fg:+-fg}.json"
                    if ! wolverine_sizes="$(python3 -I "$title_profiles/wolverine-render-preset.py" "$up_preset" "$wolverine_drs" $wolverine_fg)"; then
                        echo "[Kyty launcher] Marvel's Wolverine: the $up_preset render preset could not be prepared; the game keeps its own resolution." >&2
                        wolverine_drs=
                    else
                        [[ -n "$wolverine_fg" ]] && wolverine_fg_note=" (frame generation on: Fidelity at most 1920x1080, so it keeps 30 fps)"
                        echo "[Kyty launcher] Marvel's Wolverine: $up_preset renders Fidelity at ${wolverine_sizes% *} (to 3840x2160) and Performance at ${wolverine_sizes#* } (to 2560x1440)$wolverine_fg_note." >&2
                    fi ;;
            esac
            if [[ "$up_kind" != game && -n "$wolverine_drs" && -f "$wolverine_drs" ]]; then export KYTY_RENDER_PRESET_PATCH="$wolverine_drs"; fi
            echo "[Kyty launcher] Marvel's Wolverine: the package's tested upscaler profile ($(basename -- "$tested"))." >&2
        fi
    else
        echo "[Kyty launcher] Marvel's Wolverine: this copy's executable is not the one the package's upscaler profile was tested with; the game keeps its own upscaler." >&2
    fi
fi
if [[ "$up_state" == "available" ]]; then
    selected="$auto_binary"
    if [[ "$astro_manual" == "1" ]]; then selected="$astro_binary";
    elif [[ "$up_adapter" == taa_paths_v1 && -x "$taa_binary" ]]; then selected="$taa_binary"; fi
    export KYTY_UPSCALER_PROFILE="$up_profile" KYTY_UPSCALER="$up_kind" KYTY_UPSCALE_PRESET="$up_preset"
    if [[ -n "$up_output" ]]; then export KYTY_UPSCALE_OUTPUT="$up_output"; fi
    # Native frame generation, unless Lossless Scaling FG was chosen for this launch (one frame generator only).
    if [[ "$fg_mode" == off && ( "$up_fg" == off || ( -n "$up_fg" && "$up_fg_state" == available ) ) ]]; then export KYTY_FRAMEGEN="$up_fg"; fi
    auto_logs="$beta_data/logs"
    mkdir -p "$auto_logs"
    export KYTY_NATIVE_UPSCALER_LOG="$auto_logs/frontend-$(date +%Y%m%d-%H%M%S).jsonl"
# Without a profile (scanner unavailable or failed): the previous rule, Ghost of Yotei and its validated build.
elif [[ -z "$up_profile" && "$up_build" == "1" && "$mode" == "upstream" && "$content_title" == "PPSA26344" && -x "$upscaler_binary" ]]; then
    selected="$upscaler_binary"
    case "$up_kind" in auto) up_kind=hlx_fsr4 ;; esac # (that build has no automatic mode)
    case "$up_fg" in auto) up_fg= ;; esac
    export KYTY_UPSCALER="$up_kind" KYTY_UPSCALE_PRESET="$up_preset"
    if [[ -n "$up_output" ]]; then export KYTY_UPSCALE_OUTPUT="$up_output"; fi
    # Native frame generation, unless Lossless Scaling FG was chosen for this launch (one frame generator only).
    if [[ -n "$up_fg" && "$fg_mode" == off ]]; then export KYTY_FRAMEGEN="$up_fg"; fi
    mkdir -p "$beta_data/logs"
    export KYTY_NATIVE_UPSCALER_LOG="$beta_data/logs/frontend-$(date +%Y%m%d-%H%M%S).jsonl"
fi

# Some TAA titles expose their render size through startup constants rather than a live DRS controller. Apply
# the scanned profile's selected preset before render-graph construction, independently of --game-patch cheats.
if [[ ( "$selected" == "$auto_binary" || "$selected" == "$taa_binary" ) && -n "$up_profile" && "$up_kind" != game &&
      "$content_title" != PPSA03671 ]]; then
    preset_tool="$(dirname -- "$scanner")/kyty-render-preset"
    if [[ -f "$preset_tool" ]]; then
        if preset_plan="$(python3 -I "$preset_tool" --profile "$up_profile" --preset "$up_preset")"; then
            if [[ -n "$preset_plan" ]]; then export KYTY_RENDER_PRESET_PATCH="$preset_plan"; fi
        else
            # Without its render size the HLX route would run at the game's full resolution (on Demon's Souls that
            # did not fit in GPU memory together with frame generation). Start the game with its own upscaler
            # instead of refusing the launch.
            echo "[Kyty launcher] The $up_preset render preset could not be prepared for this game; using the game's own upscaler for this launch." >&2
            up_kind=game
            export KYTY_UPSCALER=game
        fi
    fi
fi

# Recovery belongs to the title/option, independently of later upscaler backend selection.
if [[ "$mode" == upstream && "$content_title" == PPSA01341 && "$scream_requested" == 1 ]]; then
    export KYTY_DEMONS_SCREAM_ASSERT_RECOVERY=1 KYTY_BC250_TITLE_ID=PPSA01341
elif [[ "$mode" == upstream ]]; then
    unset KYTY_DEMONS_SCREAM_ASSERT_RECOVERY
fi

export KYTY_HLX_HOME="$beta_data/upscalers" KYTY_HLX_PROTON="$bundle_root/proton/files"
hlx_fsr4_dll="$beta_data/upscalers/dlls/hlx/amd_fidelityfx_upscaler_dx12.dll"
hlx_dlss_dll="$beta_data/upscalers/dlls/hlx-dlss/helixsr.dll"
if [[ "${KYTY_UPSCALER:-game}" == hlx_dlss_e && ! -f "$hlx_dlss_dll" ]]; then
    echo '[Kyty launcher] HLX DLSS is not built on this PC (run AppRun --setup-helix-dlss); using HLX FSR 4 for this launch.' >&2
    export KYTY_UPSCALER=hlx_fsr4
fi
if [[ "${KYTY_UPSCALER:-game}" == hlx_* || "${KYTY_UPSCALER:-game}" == auto ]] && [[ ! -f "$hlx_fsr4_dll" ]]; then
    echo "[Kyty launcher] HLX FSR 4 is missing from $beta_data/upscalers/dlls; using the game's own upscaler for this launch." >&2
    export KYTY_UPSCALER=game
fi
# Keep the legacy backends launchable when a profile enables the newer ray-tracing option.
# An activity is an explicit launch request, so report when the chosen backend cannot open it.
case "${selected##*/}" in
    kyty_emulator_astro|kyty_emulator_audio|kyty_emulator_guard|kyty_emulator_fast|kyty_emulator_fast_guard)
        args=()
        for arg in "$@"; do
            case "$arg" in
                --native-rt)
                    echo '[Kyty launcher] The selected legacy backend does not support native ray tracing.' >&2 ;;
                --launch-activity)
                    echo '[Kyty launcher] The selected legacy backend cannot launch activities. Use a current native or upscaler backend.' >&2
                    exit 78 ;;
                *) args+=("$arg") ;;
            esac
        done
        set -- "${args[@]}"
        ;;
esac

# Log the final choice after every backend override. The frontend captures stderr
# in the game's log, so a crash cannot leave us guessing which executable ran.
# These are requested settings; the backend reports what actually initializes.
backend_fingerprint="$(sha256sum -- "$bundle_root/emulator/elf/${selected##*/}" 2>/dev/null)" || backend_fingerprint=unavailable
printf '[Kyty launcher] Backend: %s\n[Kyty launcher] Backend SHA-256: %s\n' \
    "$selected" "${backend_fingerprint%% *}" >&2
printf '[Kyty launcher] Profile: %s; upscaler: %s; preset: %s; native frame generation: %s; Lossless Scaling: %s\n' \
    "$mode" "${KYTY_UPSCALER:-backend-default}" "${KYTY_UPSCALE_PRESET:-backend-default}" \
    "${KYTY_FRAMEGEN:-backend-default}" "$fg_mode" >&2

if [[ "$dry_run" == "1" ]]; then
    printf 'Profile: %s\nExecutable: %s\nLossless Scaling frame generation: %s\nNative frame generation: %s\n' "$mode" "$selected" "$fg_mode" "${KYTY_FRAMEGEN:-off}"
    printf 'Upscaler profile: %s (upscaler %s, frame generation %s)\n' "${up_profile:-none}" "${up_state:-n/a}" "${up_fg_state:-n/a}"
    printf 'Argument: %q\n' "$@"
    printf 'Remaining Kyty/LSFG environment overrides:\n'
    env | grep -E '^(KYTY_|MAKO_|ENABLE_MAKO|DISABLE_MAKO|NODEVICE_SELECT=|ENABLE_LSFGVK|DISABLE_LSFGVK|LSFGVK_|VK_ADD_IMPLICIT_LAYER_PATH=|VK_LOADER_DEBUG=)' || printf '(none)\n'
    exit 0
fi

if [[ ! -x "$selected" ]]; then
    printf 'Kyty profile %s: missing executable %s\n' "$mode" "$selected" >&2
    exit 127
fi
# The official release expects its own assets, Qt libraries, saves and caches
# relative to the package directory; SharpEmu normally starts from its bridge.
if [[ "$mode" == "upstream" ]]; then
    # Both stock and separately guarded upstream variants MUST use the same
    # game data directory.  Kyty resolves _SaveData relative to its CWD.
    # The guarded ELF lives in a different release folder; using its own
    # directory hides the user's existing saves.
    cd "$upstream_data"
fi
# The emulator built from Superkeka's 2026-11-13 main (native and the upscaler family) no longer has
# --shader-optimization-type, which the launcher still sends; it would refuse to start. The older backends keep it.
case "$(basename -- "$selected")" in
    kyty_emulator_native|kyty_emulator_native_upscaler|kyty_emulator_auto_upscaler|kyty_emulator_launcher_taa_fg|kyty_emulator_upscaling)
        args=()
        while (($#)); do
            case "$1" in
                --shader-optimization-type) shift; (($#)) && shift ;;
                --shader-optimization-type=*) shift ;;
                *) args+=("$1"); shift ;;
            esac
        done
        set -- "${args[@]}" ;;
esac
if [[ -n "${KYTYPS5_LABWC_CONFIG:-}" ]]; then
    launch_game=0
    session_hdr=off
    for arg in "$@"; do
        case "$arg" in
            --game) launch_game=1 ;;
            --hdr) session_hdr=on ;;
            --guest-exec) break ;;
        esac
    done
    if [[ "$launch_game" == 1 ]]; then
        "$bundle_root/bin/python3" -I "$bundle_root/session-hdr.py" "$session_hdr"
    fi
fi
exec "$selected" "$@"
