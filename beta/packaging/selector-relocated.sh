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
    if [[ "$scream_title" == "PPSA01341" && "$scream_requested" == "1" ]]; then
        selected="$upstream_guard"
        if [[ "$audio_test" == "dialogue" ]]; then
            selected="$audio_binary"
        fi
    fi

    # The custom SharpEmu bridge currently exports KFD=0, partial GEM residency,
    # skip-shader controls, and other experiments. NONE are allowed to reach the
    # native backend. The game's native GPU path selects its own defaults.
    while IFS= read -r name; do
        case "$name" in
            KYTY_*|ENABLE_LSFGVK|LSFGVK_*) unset "$name" ;;
        esac
    done < <(compgen -e)
    if [[ "$selected" == "$upstream_guard" || "$selected" == "$audio_binary" ]]; then
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
# User-requested Yotei content experiment: register all 15 catalogue labels.
# These are license-only entries; no extra content directories are implied.
if [[ "$mode" == "upstream" && "$content_title" == "PPSA26344" ]]; then
    set -- "$@" --addcont GHOST2APP0000000
    set -- "$@" --addcont GHOST2BASE000000
    set -- "$@" --addcont GHOST2BASEME0000
    set -- "$@" --addcont GHOST2CEUPGRADE0
    set -- "$@" --addcont GHOST2COLLECTORS
    set -- "$@" --addcont GHOST2DDE0000000
    set -- "$@" --addcont GHOST2DISC000000
    set -- "$@" --addcont GHOST2DLC0000000
    set -- "$@" --addcont GHOST2DLCPREPURC
    set -- "$@" --addcont GHOST2DLCREVIEW0
    set -- "$@" --addcont GHOST2LEGENDS000
    set -- "$@" --addcont GHOST2LEGENDSUPG
    set -- "$@" --addcont GHOST2PREORDER00
    set -- "$@" --addcont GHOST2SAPPORO000
    set -- "$@" --addcont GHOST2SHIP000000
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
    bound="$(python3 -I "$bundle_root/prepare-runtime.py" --astro "$astro_game_arg")" || exit 2
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
            case "$up_preset" in
                ultra_quality)
                    up_profile="$astro_root/astro-ultra-quality.json"
                    export KYTY_RENDER_PRESET_PATCH="$astro_root/astro-ultra-quality-render.json" ;;
                dlaa)
                    up_profile="$astro_root/astro-native-dlaa.json"
                    export KYTY_RENDER_PRESET_PATCH="$astro_root/astro-native-dlaa-render.json" ;;
                *) up_preset=dynamic ;;
            esac
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
        up_profile="$(python3 -I "$scanner" ensure "$scan_path" 2>/dev/null)" || up_profile=
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
if [[ ( "$selected" == "$auto_binary" || "$selected" == "$taa_binary" ) && -n "$up_profile" && "$up_kind" != game ]]; then
    preset_tool="$(dirname -- "$scanner")/kyty-render-preset"
    if [[ -f "$preset_tool" ]]; then
        preset_plan="$(python3 -I "$preset_tool" --profile "$up_profile" --preset "$up_preset")" || exit 2
        if [[ -n "$preset_plan" ]]; then export KYTY_RENDER_PRESET_PATCH="$preset_plan"; fi
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
exec "$selected" "$@"
