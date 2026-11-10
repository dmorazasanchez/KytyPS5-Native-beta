#!/usr/bin/env bash
# Shared native builds reserve and verify their graphics VMID before title GPU work.
# Enabled in the backend for every game; no per-title or experimental environment switch.
# All previous binary experiments remain available only through the Fast profile.
# The upstream path removes unrelated SharpEmu/BC-250 experiment overrides.
set -euo pipefail

mode="${KYTY_NATIVE_GPU_PM4_MODE:-fast}"
dry_run="${KYTY_EMULATOR_SELECTOR_DRY_RUN:-0}"
# Presentation-only choice from the SharpEmu frontend. Never applied to the
# native renderer itself; only the host Vulkan presentation layer.
fg_mode="${KYTY_SHARP_FG_MODE:-off}"
scream_requested="${KYTY_DEMONS_SCREAM_ASSERT_RECOVERY:-0}"
scream_title="${KYTY_BC250_TITLE_ID:-}"
audio_test="${KYTY_BC250_AUDIO_TEST:-off}"
audio_binary=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_audio
case "$fg_mode" in off|fixed|adaptive) ;; *) fg_mode=off ;; esac
unset KYTY_SHARP_FG_MODE
# Native upscaler build (copy of the selector for the upscaling frontend build): captured before the KYTY_* strip.
up_build="${KYTY_NATIVE_UPSCALER_BUILD:-0}"; up_kind="${KYTY_UPSCALER:-game}"; up_preset="${KYTY_UPSCALE_PRESET:-dynamic}"
up_output="${KYTY_UPSCALE_OUTPUT:-}"; case "$up_output" in 1800|1440) ;; *) up_output= ;; esac
up_fg="${KYTY_FRAMEGEN:-}"; case "$up_fg" in off|fsr3|fsr3_dynamic|auto) ;; *) up_fg= ;; esac
case "$up_kind" in auto|game|hlx_fsr4|hlx_dlss_e) ;; *) up_kind=game ;; esac
unset KYTY_NATIVE_UPSCALER_BUILD KYTY_UPSCALER KYTY_UPSCALE_PRESET KYTY_UPSCALE_OUTPUT KYTY_FRAMEGEN KYTY_RENDER_PRESET_PATCH
# The validated copy (development builds use other names, so they never change what the launcher runs).
upscaler_binary=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_native_upscaler
unset ENABLE_LSFGVK ENABLE_LSFGVK_EXPERIMENTAL LSFGVK_CONFIG LSFGVK_PROFILE LSFGVK_ENV
unset VK_ADD_IMPLICIT_LAYER_PATH
# Presentation settings are opt-in for each launch, including inherited MAKO settings.
while IFS= read -r name; do
    case "$name" in MAKO_*|ENABLE_MAKO*|DISABLE_MAKO*) unset "$name" ;; esac
done < <(compgen -e)
export DISABLE_MAKO=1 DISABLE_MAKO_SPATIAL_SCALING=1
export DISABLE_LSFGVK=1 DISABLE_LSFGVK_EXPERIMENTAL=1

upstream=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_native
upstream_data=/home/david/kyty-upstream-clean-47857b4-20261104/release
upstream_guard=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_guard
safe=/home/david/kyty-native-release-160251b-20261029/kyty_emulator
safe_guard=/home/david/kyty-native-release-160251b-20261029/kyty_emulator_demons1007_screamguard_6d3e578b

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
if [[ "$mode" == "upstream" && -z "$content_title" ]]; then
    # The current frontend strips the title environment marker for ordinary
    # Upstream launches. Recover it from this game's metadata for the existing
    # content registrations, without changing renderer/recovery selection.
    content_title="$(python3 - "$@" <<'PY'
import json
from pathlib import Path
import sys

args = sys.argv[1:]
try:
    game = Path(args[args.index('--game') + 1])
    root = game if game.is_dir() else game.parent
    metadata = json.loads((root / 'sce_sys' / 'param.json').read_text())
    title = metadata.get('titleId', '') if isinstance(metadata, dict) else ''
    if isinstance(title, str):
        print(title)
except (ValueError, IndexError, OSError, UnicodeError):
    pass
PY
)"
fi

# Explicitly scoped Vulkan-layer injection. The selected emulator ELF and its
# GPU command parameters are identical with FG on or off.
kyty_user_home="${HOME:?}"
fixed_config="$kyty_user_home/.config/lsfg-vk/kyty-bc250-2x-verified.toml"
adaptive_config="$kyty_user_home/.config/lsfg-vk/kyty-bc250-adaptive.toml"
adaptive_layer="$kyty_user_home/.local/opt/lsfg-vk-experimental/share/vulkan/implicit_layer.d"
case "$fg_mode" in
    fixed)
        if [[ ! -f "$fixed_config" ]]; then
            echo "Fixed LSFG profile missing: $fixed_config" >&2; exit 2
        fi
        # SharpEmu has an isolated XDG_DATA_HOME, so Vulkan does not discover
        # the user's normal ~/.local/share/vulkan/implicit_layer.d directory.
        # Add the LSFG manifest directory explicitly without changing XDG_DATA_HOME.
        fixed_layer="$kyty_user_home/.local/share/vulkan/implicit_layer.d"
        if [[ ! -f "$fixed_layer/VkLayer_LSFGVK_frame_generation.json" ]]; then
            echo "Fixed LSFG Vulkan layer missing: $fixed_layer" >&2; exit 2
        fi
        # Vulkan disables an implicit layer if its DISABLE_ variable exists,
        # even when set to "0". Unset it to actually enable this layer.
        unset DISABLE_LSFGVK
        export ENABLE_LSFGVK=1
        export LSFGVK_CONFIG="$fixed_config"
        # The upscaler/recovery binaries use different executable names. Select
        # the same validated profile explicitly instead of relying on active_in.
        export LSFGVK_PROFILE="Kyty BC-250 verified 2x"
        export VK_ADD_IMPLICIT_LAYER_PATH="$fixed_layer"
        export VK_LOADER_DEBUG=layer
        ;;
    adaptive)
        if [[ "$mode" == "upstream" && "$content_title" == "PPSA26344" ]]; then
            source /home/david/sharpemu-upscaling-20261108/kyty-yotei-lsfg-dynamic.sh
        else
            if [[ ! -f "$adaptive_config" || ! -f "$adaptive_layer/VkLayer_LSFGVK_experimental_frame_generation.json" ]]; then
                echo "Adaptive LSFG runtime missing; cannot enable FG." >&2; exit 2
            fi
            unset DISABLE_LSFGVK_EXPERIMENTAL
            export ENABLE_LSFGVK_EXPERIMENTAL=1
            export LSFGVK_CONFIG="$adaptive_config"
            export LSFGVK_PROFILE="Kyty BC-250 Adaptive 60 FPS"
            export VK_ADD_IMPLICIT_LAYER_PATH="$adaptive_layer"
            export VK_LOADER_DEBUG=layer
        fi
        ;;
esac
if [[ "$fg_mode" != off ]]; then export KYTY_FRAMEGEN=off; fi
# Only on FG launches: diagnose whether the Vulkan loader enables the LSFG layer.
# SharpEmu captures stderr in the existing per-game emulator log.
if [[ "$fg_mode" != off ]]; then
    printf '[KytyPS5 LSFG diagnostic] mode=%s config=%s layer_dir=%s VK_LOADER_DEBUG=%s\n' \
        "$fg_mode" "${MAKO_CONFIG:-${LSFGVK_CONFIG:-}}" "${VK_ADD_IMPLICIT_LAYER_PATH:-default}" "$VK_LOADER_DEBUG" >&2
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
auto_binary=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_auto_upscaler
taa_binary=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_launcher_taa_fg
scanner="${KYTYPS5_UPSCALER_SCANNER:-/home/david/kyty-auto-upscaler-20261109/tools/upscaler-scan/kyty-upscaler-scan}"
up_profile= up_state= up_fg_state= up_adapter=
# Manual Astro integration: exact installed archive and stamp; no scanner/discovery on this route.
astro_root=/home/david/astro-helix-framegen-20261110/manual
astro_binary=/home/david/kyty-native-vmid-20261110/release/20261110-recovery/kyty_emulator_astro
astro_manual=0
if [[ "$up_build" == "1" && "$mode" == upstream && -f "$astro_root/astro-manual.json" ]]; then
    astro_game_arg= prev=
    for a in "$@"; do
        if [[ "$prev" == --game ]]; then astro_game_arg="$a"; fi
        prev="$a"
    done
    if python3 -I - "$astro_game_arg" "$astro_root/astro-manual.json" <<'CHECK_ASTRO'
import json, sys
from pathlib import Path
try:
    p=json.loads(Path(sys.argv[2]).read_text()); g=p['game']; loc=g['location']
    archive=Path(loc['archive']); expected=str(archive)+'!/'+loc['member'].rsplit('/',1)[0]
    arg=sys.argv[1].removesuffix('/eboot.bin').rstrip('/')
    st=archive.stat()
    valid=arg==expected and f'{st.st_size}|{st.st_mtime_ns}'==g['stamp']
    sys.exit(0 if valid else 1)
except (OSError,ValueError,KeyError): sys.exit(1)
CHECK_ASTRO
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
    auto_logs=/home/david/kyty-auto-upscaler-20261109/experiments/auto-upscaler/logs
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
    mkdir -p /home/david/kyty-native-upscaling-20261108/experiments/native-upscaling/logs
    export KYTY_NATIVE_UPSCALER_LOG="/home/david/kyty-native-upscaling-20261108/experiments/native-upscaling/logs/frontend-$(date +%Y%m%d-%H%M%S).jsonl"
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

# Log the final choice after every backend override. The frontend captures stderr
# in the game's log, so a crash cannot leave us guessing which executable ran.
# These are requested settings; the backend reports what actually initializes.
backend_fingerprint="$(sha256sum -- "$selected" 2>/dev/null)" || backend_fingerprint=unavailable
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
