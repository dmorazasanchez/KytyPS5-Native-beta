#!/usr/bin/env python3
"""Install beta-owned helper/session files under /var and /etc, without changing boot/kernel settings."""
import argparse, datetime, json, os, pwd, re, shlex, shutil, subprocess
from pathlib import Path

SYSTEM = Path('/var/lib/kyty-beta-system')
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=['--install-helper','--install-gaming-mode','--uninstall-system']) if False else None
p.add_argument('--install-helper', action='store_true')
p.add_argument('--install-gaming-mode', action='store_true')
p.add_argument('--uninstall-system', action='store_true')
p.add_argument('--payload', type=Path)
p.add_argument('--launcher', type=Path)
p.add_argument('--user')
p.add_argument('--dry-run', action='store_true')
p.add_argument('--output', type=Path, help='Write generated files here during --dry-run')
a = p.parse_args()
source = a.payload or Path(__file__).resolve().parent
account = pwd.getpwnam(a.user) if a.user else pwd.getpwuid(int(os.environ.get('SUDO_UID', os.getuid())))
if account.pw_uid == 0 and not a.uninstall_system: p.error('Choose the tester account with --user USER when running directly as root')
if not re.fullmatch(r'[a-zA-Z_][a-zA-Z0-9_.-]*\$?', account.pw_name): p.error('Unsupported account name')
launcher = (a.launcher or source / 'AppRun').resolve()
if any(c in str(launcher) for c in '\n\r%'): p.error('Launcher path cannot contain newlines or percent signs')

def generated_files():
    files = {}
    def add(path, content, mode=0o644): files[str(path)] = (content.encode() if isinstance(content,str) else content, mode)
    if a.install_helper:
        add(SYSTEM / 'kyty-lodhw', (source / 'system/kyty-lodhw').read_bytes(), 0o755)
        add('/etc/systemd/system/kyty-lodhw.service', '[Unit]\nDescription=Kyty BC-250 GPU helper (beta)\nAfter=systemd-udev-settle.service\nWants=systemd-udev-settle.service\n[Service]\nType=simple\nExecStart=' + str(SYSTEM) + '/kyty-lodhw /run/kyty-lodhw.sock ' + str(account.pw_gid) + '\nRestart=on-failure\nRestartSec=2\n[Install]\nWantedBy=multi-user.target\n')
    if a.install_gaming_mode:
        original = source / 'system/session'
        home = account.pw_dir
        prefix = str(SYSTEM / 'session')
        for f in original.iterdir():
            if not f.is_file() or f.name in ('install-ram-audit.py','ps5-ram-audit','ps5-ram-compare','40-memory-audit.conf','org.bc250.ps5.minimal.policy','ps5-minimal.desktop','ps5-minimal-mode.desktop'): continue
            text = f.read_text()
            text = text.replace('/home/david/sharpemu-upscaling-20261108/run-kytyps5.sh', shlex.quote(str(launcher)))
            text = text.replace('/home/david/sharpemu-upscaling-20261108/artifacts/kytyps5/KytyPS5', str(SYSTEM / 'launch-user'))
            text = text.replace('/usr/local/libexec/', prefix + '/').replace('/usr/local/bin/ps5-minimal-start', prefix + '/ps5-minimal-start')
            text = text.replace('USER = "david"', 'USER = ' + repr(account.pw_name)).replace('UID = 1000', 'UID = ' + str(account.pw_uid))
            text = text.replace('/run/user/1000/', '/run/user/' + str(account.pw_uid) + '/').replace('user@1000.service', 'user@' + str(account.pw_uid) + '.service')
            text = text.replace('User=david', 'User=' + account.pw_name).replace('Group=david', 'Group=' + str(account.pw_gid))
            text = text.replace('USER=david LOGNAME=david', 'USER=' + account.pw_name + ' LOGNAME=' + account.pw_name)
            text = text.replace('/home/david', home)
            text = text.replace('"/usr/lib/steamos/steam-set-session",', '') if False else text
            if f.name == 'ps5-minimal-control':
                text = text.replace('                 Path("/usr/lib/steamos/steam-set-session")):', '                 Path("/usr/bin/pkexec")):')
                text = text.replace('    run("/usr/lib/steamos/steam-set-session", "plasma.desktop")', '    if Path("/usr/lib/steamos/steam-set-session").is_file():\n        run("/usr/lib/steamos/steam-set-session", "plasma.desktop")')
                text = text.replace("david's", account.pw_name + "'s")
            if f.name == 'ps5-minimal.pam' and not Path('/etc/pam.d/system-local-login').exists():
                text = text.replace('system-local-login', 'system-auth')
            if f.name == 'rc.xml':
                text = re.sub(r'    <keybind key="Print">.*?</keybind>\n', '', text, flags=re.S)
            if f.name.endswith(('.service','.timer','.target')):
                target = Path('/etc/systemd/system') / f.name
            elif f.name == 'ps5-minimal.pam': target = Path('/etc/pam.d/ps5-minimal')
            elif f.name == 'dbus-session.conf': target = Path('/etc/ps5-minimal/dbus-session.conf')
            elif f.name == 'rc.xml': target = Path('/etc/ps5-minimal/labwc/rc.xml')
            else: target = SYSTEM / 'session' / f.name
            add(target, text, 0o755 if target.parent == SYSTEM / 'session' else 0o644)
        add(SYSTEM / 'launch-user', '#!/usr/bin/env bash\nexec ' + shlex.quote(str(launcher)) + ' "$@"\n', 0o755)
        rule = '''polkit.addRule(function(action, subject) {
    if (action.id === "org.freedesktop.policykit.exec" &&
        action.lookup("program") === %s && subject.user === %s && subject.local && subject.active)
        return polkit.Result.YES;
});
''' % (json.dumps(prefix + '/ps5-minimal-control'), json.dumps(account.pw_name))
        add('/etc/polkit-1/rules.d/49-kyty-beta-minimal.rules', rule)
    return files

def host_preflight():
    if not launcher.is_file() or not os.access(launcher, os.X_OK): raise RuntimeError('Keep the launcher/AppImage in a permanent executable location: ' + str(launcher))
    if a.install_gaming_mode:
        required = ['labwc','Xwayland','xdotool','dbus-run-session','pipewire','pipewire-pulse','wireplumber','pkexec','systemctl','loginctl']
        missing = [n for n in required if not shutil.which(n)]
        if missing: raise RuntimeError('Minimal session dependencies missing: ' + ', '.join(missing) + '. Use the desktop/Steam Game Mode launch path until these are installed.')
        version = subprocess.check_output(['systemctl','--version'], text=True).split()[1]
        if int(version) < 256: raise RuntimeError('Minimal session needs systemd >= 256 for user-light PAM sessions')
        if not Path('/etc/pam.d/system-local-login').exists() and not Path('/etc/pam.d/system-auth').exists(): raise RuntimeError('No supported PAM login account policy found')

def main():
    if not any((a.install_helper,a.install_gaming_mode,a.uninstall_system)): p.error('Choose an action')
    if a.dry_run:
        files = generated_files() if not a.uninstall_system else {}
        if a.output:
            for path, (data, mode) in files.items():
                dest = a.output / path.lstrip('/'); dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data); dest.chmod(mode)
        print(json.dumps({'user':account.pw_name,'uid':account.pw_uid,'gid':account.pw_gid,'launcher':str(launcher),'files':list(files),'system_changed':False}, indent=2))
        return
    if os.geteuid() != 0: raise RuntimeError('Use AppRun --install-helper or --install-gaming-mode (sudo required)')
    manifest_file = SYSTEM / 'install-manifest.json'
    if a.uninstall_system:
        if Path('/run/ps5-minimal/state.json').exists(): raise RuntimeError('Return to desktop before uninstalling the session')
        if not manifest_file.exists(): raise RuntimeError('No beta system installation manifest found')
        manifest = json.loads(manifest_file.read_text())
        if '/etc/systemd/system/kyty-lodhw.service' in manifest['files']:
            subprocess.run(['systemctl','disable','--now','kyty-lodhw.service'], check=False)
        for path, entry in reversed(list(manifest['files'].items())):
            dest = Path(path)
            if entry['backup']:
                shutil.copy2(entry['backup'], dest)
            else: dest.unlink(missing_ok=True)
        subprocess.run(['systemctl','daemon-reload'], check=True)
        if manifest.get('helper_was_enabled'): subprocess.run(['systemctl','enable','kyty-lodhw.service'], check=True)
        if manifest.get('helper_was_active'): subprocess.run(['systemctl','start','kyty-lodhw.service'], check=True)
        manifest_file.rename(SYSTEM / ('uninstalled-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '.json'))
        print('Removed beta system files and restored backed-up definitions. Beta saves/settings retained.')
        return
    host_preflight()
    files = generated_files()
    SYSTEM.mkdir(parents=True, exist_ok=True)
    SYSTEM.chmod(0o755)
    manifest = json.loads(manifest_file.read_text()) if manifest_file.exists() else {'files':{}, 'helper_was_active': subprocess.run(['systemctl','is-active','--quiet','kyty-lodhw.service']).returncode == 0, 'helper_was_enabled': subprocess.run(['systemctl','is-enabled','--quiet','kyty-lodhw.service']).returncode == 0}
    backup = SYSTEM / 'backups' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    for path, (content, mode) in files.items():
        dest = Path(path)
        if path not in manifest['files']:
            saved = None
            if dest.exists() or dest.is_symlink():
                target = backup / path.lstrip('/'); target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(dest, target, follow_symlinks=False); saved = str(target)
            manifest['files'][path] = {'backup':saved}
            manifest_file.write_text(json.dumps(manifest, indent=2) + '\n')
        dest.parent.mkdir(parents=True, exist_ok=True)
        temporary = dest.with_name(dest.name + '.kyty-beta-new')
        temporary.write_bytes(content); temporary.chmod(mode); os.chown(temporary,0,0); temporary.replace(dest)
    subprocess.run(['systemctl','daemon-reload'], check=True)
    if a.install_helper:
        subprocess.run(['systemctl','enable','kyty-lodhw.service'], check=True)
        subprocess.run(['systemctl','restart','kyty-lodhw.service'], check=True)
    if a.install_gaming_mode:
        print('Minimal mode installed but not started. It closes your login/apps and stops background services while active.')
        print('Enter from a local desktop: pkexec ' + str(SYSTEM / 'session/ps5-minimal-control') + ' enter')
        print('Recovery: Ctrl+Alt+F4, log in, then sudo ' + str(SYSTEM / 'session/ps5-minimal-control') + ' desktop')
        print('On hosts without Valve steam-set-session, returning restores the configured display-manager session/login screen.')
    print('System files use /var/lib and /etc. No read-only root unlock, boot change, or kernel change was made.')

if __name__ == '__main__':
    try: main()
    except Exception as e: raise SystemExit(str(e))
