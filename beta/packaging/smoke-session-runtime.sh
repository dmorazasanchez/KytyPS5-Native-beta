#!/usr/bin/env bash
# Run as an unprivileged user with the AppDir mounted read-only at /app.
set -euo pipefail
test ! -x /usr/bin/labwc
test ! -x /usr/bin/xdotool
work=$(mktemp -d)
export XDG_RUNTIME_DIR="$work/run"
mkdir -m700 "$XDG_RUNTIME_DIR"
export KYTYPS5_CONFIG_ROOT="$work/config" KYTYPS5_DATA_ROOT="$work/data" KYTYPS5_PROFILE_ROOT="$work/profile"
export KYTYPS5_SESSION_HEALTH="$XDG_RUNTIME_DIR/health.json" PS5_MINIMAL_SESSION=1
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman
export SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
/app/bin/python3 /app/install-system.py --install-gaming-mode --dry-run --output "$work/install" > "$work/generated.json"
export session_tools="$work/install/var/lib/kyty-beta-system/session-runtime"
grep -q 'ConditionFileIsExecutable=.*session-runtime/labwc' "$work/install/etc/systemd/system/ps5-minimal-session.service"
grep -q 'session-runtime/xdotool' "$work/install/var/lib/kyty-beta-system/session/ps5-minimal-launcher"
cat > "$work/launch" <<'SESSION'
#!/usr/bin/env bash
set -euo pipefail
/app/AppRun &
pid=$!
trap 'kill "$pid" 2>/dev/null || true' EXIT
window=
for attempt in {1..100}; do
    window=$("$session_tools/xdotool" search --onlyvisible --pid "$pid" 2>/dev/null | head -n1) || true
    [[ -n "$window" ]] && break
    kill -0 "$pid"
    sleep .1
done
[[ -n "$window" ]]
"$session_tools/xdotool" windowactivate --sync "$window"
"$session_tools/xdotool" windowfocus --sync "$window"
for attempt in {1..100}; do
    if /app/bin/python3 - "$KYTYPS5_SESSION_HEALTH" <<'PY'
import json
from pathlib import Path
import sys
try:
    health = json.loads(Path(sys.argv[1]).read_text())
except (OSError, ValueError):
    raise SystemExit(1)
if not (health.get('ticks', 0) >= 3 and health.get('active') and health.get('loaded')):
    raise SystemExit(1)
print('PASS: Kyty window focused and responding under bundled labwc and Xwayland')
PY
    then
        touch "$XDG_RUNTIME_DIR/passed"
        exit 0
    fi
    sleep .1
done
exit 1
SESSION
chmod +x "$work/launch"
dbus-run-session -- timeout 35 "$session_tools/labwc" --config-dir "$work/install/etc/ps5-minimal/labwc" --session "$work/launch"
test -f "$XDG_RUNTIME_DIR/passed"
