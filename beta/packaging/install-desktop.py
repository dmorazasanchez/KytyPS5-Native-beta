#!/usr/bin/env python3
import os, sys
from pathlib import Path
bundle = Path(__file__).resolve().parent
launcher = Path(os.environ.get('APPIMAGE', str(bundle / 'AppRun'))).resolve()
if '\n' in str(launcher) or '\r' in str(launcher): raise SystemExit('Unsupported newline in launcher path')
def quote(s): return '"' + str(s).replace('\\','\\\\').replace('"','\\"').replace('`','\\`').replace('$','\\$').replace('%','%%') + '"'
base = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share')))
icon = base / 'icons/hicolor/256x256/apps/kytyps5-beta.png'
icon.parent.mkdir(parents=True, exist_ok=True)
icon.write_bytes((bundle / 'kytyps5.png').read_bytes())
dest = base / 'applications/kytyps5-beta.desktop'
if '--remove' in sys.argv:
    dest.unlink(missing_ok=True); icon.unlink(missing_ok=True)
    (base / 'applications/kytyps5-beta-minimal.desktop').unlink(missing_ok=True)
    print('Beta shortcuts removed.'); raise SystemExit(0)
dest.parent.mkdir(parents=True, exist_ok=True)
dest.write_text('[Desktop Entry]\nType=Application\nName=KytyPS5 Beta\nExec=' + quote(launcher) + '\nIcon=kytyps5-beta\nCategories=Game;Emulator;\nTerminal=false\nStartupWMClass=KytyPS5\n')
if '--gaming-mode' in sys.argv:
    (base / 'applications/kytyps5-beta-minimal.desktop').write_text('[Desktop Entry]\nType=Application\nName=KytyPS5 Beta Minimal Mode\nComment=Closes this login and starts the minimal emulator session\nExec=/usr/bin/pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control enter\nIcon=kytyps5-beta\nCategories=Game;\nTerminal=false\n')
print('Installed: ' + str(dest))
print('Keep the AppImage/folder at: ' + str(launcher))
print('For Steam Gaming Mode, add KytyPS5 Beta as a non-Steam game and disable Proton compatibility for this Linux application.')
