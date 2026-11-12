#!/usr/bin/env bash
# PS5 Minimal Mode for KytyPS5 BC-250: a low-RAM game session (labwc + the KytyPS5 launcher, no desktop, no
# background services) that you enter from the desktop and leave back to it, without rebooting.
#
#   ./install.sh                       install for the KytyPS5 found on this PC (asks for your password)
#   ./install.sh --launcher PATH       install for this KytyPS5 AppImage or AppRun
#   ./install.sh --check               show what would be installed, change nothing
#   ./install.sh --uninstall           remove Minimal Mode (and the GPU helper; saves/settings are kept)
#
# The same installer works on SteamOS and on CachyOS/Arch: it detects the system and picks the matching setup.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
user="$(id -un)"
[[ $EUID != 0 ]] || { echo "Run this as your normal user (it asks for your password when needed), not as root." >&2; exit 2; }

launcher=
mode=install
while [[ $# -gt 0 ]]; do
    case "$1" in
        --launcher) launcher="$(realpath -- "$2")"; shift ;;
        --check) mode=check ;;
        --uninstall) mode=uninstall ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

if [[ $mode == uninstall ]]; then
    exec sudo -- /usr/bin/python3 "$here/install-system.py" --uninstall-system --user "$user"
fi

find_launcher() {
    # 1. the menu entry KytyPS5's --install-desktop wrote
    local entry="${XDG_DATA_HOME:-$HOME/.local/share}/applications/kytyps5-beta.desktop"
    if [[ -f "$entry" ]]; then
        local exec_line; exec_line="$(sed -n 's/^Exec=//p' "$entry" | head -n 1)"
        exec_line="${exec_line#\"}"; exec_line="${exec_line%\"}"
        if [[ -x "$exec_line" ]]; then printf '%s\n' "$exec_line"; return; fi
    fi
    # 2. the newest KytyPS5 AppImage or extracted folder in the usual places
    local found
    found="$(find "$HOME/Applications" "$HOME/Downloads" "$HOME/Desktop" "$HOME" -maxdepth 3 \
        \( -name 'KytyPS5-BC250-*-x86_64.AppImage' -o -path '*KytyPS5-BC250-*-x86_64/AppRun' \) \
        -type f -executable -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n 1 | cut -d' ' -f2-)" || true
    [[ -n "$found" ]] && printf '%s\n' "$found"
}
[[ -n "$launcher" ]] || launcher="$(find_launcher || true)"
if [[ -z "$launcher" || ! -x "$launcher" ]]; then
    echo "KytyPS5 was not found. Download it, run it once with --install-desktop, or pass --launcher /path/to/it." >&2
    exit 2
fi

# What the installer will do on this system (the same decisions install-system.py makes).
. /etc/os-release 2>/dev/null || true
echo "System:   ${PRETTY_NAME:-unknown}"
echo "KytyPS5:  $launcher"
if [[ -x /usr/bin/labwc ]] && /usr/bin/labwc --help 2>&1 | grep -q -- '--session'; then
    echo "Session:  this system's labwc"
else
    echo "Session:  bundled labwc (this system has none), with the host Wayland client library for host Mesa"
fi
[[ -f /usr/lib/systemd/system/systemd-userdbd.service ]] && echo "Login:    systemd-userdbd kept running during the switch (needed on SteamOS)"
[[ -f /usr/lib/steamos/steam-set-session ]] && echo "Return:   back to Desktop Mode (SteamOS)" || echo "Return:   back to your display manager's login/session"
missing=()
for tool in Xwayland dbus-run-session pipewire pipewire-pulse wireplumber pkexec; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
if (( ${#missing[@]} )); then
    echo "Missing on this system: ${missing[*]}" >&2
    case "${ID:-}${ID_LIKE:-}" in
        *arch*|*cachyos*) echo "Install them with: sudo pacman -S --needed xorg-xwayland dbus pipewire pipewire-pulse wireplumber polkit" >&2 ;;
    esac
    exit 2
fi

if [[ $mode == check ]]; then
    out="$(mktemp -d)"; trap 'rm -rf -- "$out"' EXIT
    /usr/bin/python3 "$here/install-system.py" --install-gaming-mode --dry-run --payload "$here" \
        --launcher "$launcher" --user "$user" --output "$out"
    exit 0
fi

echo
echo "Administrator password needed to install the session:"
sudo -- /usr/bin/python3 "$here/install-system.py" --install-gaming-mode --payload "$here" --launcher "$launcher" --user "$user"

# Menu entry to start it (add it to Steam as a non-Steam game to start it from Game Mode).
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
mkdir -p "$apps" "$icons"
cp "$here/kytyps5.png" "$icons/kytyps5-beta.png"
cat > "$apps/kytyps5-beta-minimal.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=KytyPS5 PS5 Minimal Mode
Comment=Closes this desktop and starts the low-RAM KytyPS5 game session
Exec=/usr/bin/pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control enter
Icon=kytyps5-beta
Categories=Game;
Terminal=false
EOF
echo
echo "PS5 Minimal Mode is installed. Start it from the application menu: KytyPS5 PS5 Minimal Mode."
echo "Save your work first: entering closes your desktop apps. Leave with the launcher's Control centre ->"
echo "Return to Desktop Mode, or Super+Shift+E."
