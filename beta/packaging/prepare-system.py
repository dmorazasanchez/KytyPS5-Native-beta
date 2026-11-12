#!/usr/bin/env python3
"""Prepare a BC-250 Linux system for KytyPS5: the GPU driver options and GTT size the native backend needs.

  prepare-system.py --status            show what is set, active and missing (no root needed)
  prepare-system.py --apply             add only what is missing (root), then reboot
  prepare-system.py --revert            remove exactly what --apply added (root), then reboot
  prepare-system.py --apply --dry-run   show the changes without writing anything

Supported: SteamOS (modprobe file + GRUB drop-in), CachyOS/Arch with Limine (/etc/default/limine) or GRUB
(/etc/default/grub), and rpm-ostree systems such as Bazzite (kernel arguments). Values the user already set are
never overwritten. Everything added is recorded in /var/lib/kyty-beta-system/prepare-system.json for --revert.
The BIOS (512 MiB UMA/VRAM) and the GPU clock governor are reported, not changed.
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

GTT_PAGES = '3670016'                       # 14 GiB of 4 KiB pages
STOCK = {'sched_policy': '2', 'vm_update_mode': '0', 'cwsr_enable': '0'}
PATCHED = {'sched_policy': '2', 'vm_update_mode': '0'}   # the patched module sizes its own wave save area
MARK = '# KytyPS5-BC250 prepare-system'

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
g = p.add_mutually_exclusive_group(required=True)
g.add_argument('--status', action='store_true')
g.add_argument('--apply', action='store_true')
g.add_argument('--revert', action='store_true')
p.add_argument('--dry-run', action='store_true', help='print planned changes, write nothing')
p.add_argument('--json', action='store_true', help='--status as JSON')
p.add_argument('--root', type=Path, default=Path('/'), help='operate on another root (testing)')
p.add_argument('--os', choices=['steamos', 'limine', 'grub', 'ostree', 'other'], help='override detection (testing)')
a = p.parse_args()
ROOT = a.root


def R(path):
    return ROOT / str(path).lstrip('/')


def read(path):
    try:
        return R(path).read_text()
    except OSError:
        return ''


def run(*cmd, check=True):
    if a.dry_run or ROOT != Path('/'):
        print('  would run: ' + ' '.join(cmd))
        return 0, ''
    r = subprocess.run(cmd, text=True, capture_output=True)
    if check and r.returncode:
        raise RuntimeError(' '.join(cmd) + ' failed: ' + (r.stderr or r.stdout).strip()[-600:])
    return r.returncode, (r.stdout + r.stderr)


def os_release():
    info = {}
    for line in read('/etc/os-release').splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            info[k] = v.strip().strip('"')
    return info


def method():
    if a.os:
        return a.os
    if shutil.which('rpm-ostree') and R('/run/ostree-booted').exists():
        return 'ostree'
    info = os_release()
    if info.get('ID') == 'steamos':
        return 'steamos'
    if R('/etc/default/limine').is_file():
        return 'limine'
    if R('/etc/default/grub').is_file() and (shutil.which('grub-mkconfig') or shutil.which('update-grub')):
        return 'grub'
    return 'other'


def patched_module():
    return R('/sys/module/amdgpu/parameters/bc250_gfx_queues').exists()


def wanted():
    want = dict(PATCHED if patched_module() else STOCK)
    return want


def active(name):
    return read('/sys/module/amdgpu/parameters/' + name).strip()


def modprobe_options():
    opts = {}
    for f in sorted(glob.glob(str(R('/etc/modprobe.d')) + '/*.conf')) + sorted(glob.glob(str(R('/usr/lib/modprobe.d')) + '/*.conf')):
        for line in Path(f).read_text(errors='replace').splitlines():
            parts = line.split('#', 1)[0].split()
            if len(parts) >= 3 and parts[0] == 'options' and parts[1] == 'amdgpu':
                for kv in parts[2:]:
                    if '=' in kv:
                        k, v = kv.split('=', 1)
                        opts.setdefault(k, (v, f.replace(str(ROOT).rstrip('/'), '', 1) if ROOT != Path('/') else f))
    return opts


def cmdline_args(text):
    out = {}
    for tok in text.split():
        tok = tok.strip('"\'')
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = v.strip('"\'')
    return out


def configured_cmdline(m):
    """Kernel arguments the NEXT boot will use, as far as the bootloader config tells."""
    if m == 'ostree':
        if ROOT != Path('/'):
            return {}
        r = subprocess.run(['rpm-ostree', 'kargs'], text=True, capture_output=True)
        return cmdline_args(r.stdout) if r.returncode == 0 else {}
    if m == 'limine':
        merged = {}
        for key, value in limine_entries(read('/etc/default/limine')).items():
            for k, v in cmdline_args(value).items():
                merged.setdefault(k, v)
        return merged
    if m in ('steamos', 'grub'):
        text = read('/etc/default/grub') + '\n' + read('/etc/default/grub-steamos')
        for f in sorted(glob.glob(str(R('/etc/default/grub.d')) + '/*.cfg')):
            text += '\n' + Path(f).read_text(errors='replace')
        return cmdline_args(' '.join(re.findall(r'"([^"]*)"', text)) + ' ' + text)
    return {}


def limine_entries(text):
    """KERNEL_CMDLINE[name] -> concatenated value (= and += lines)."""
    entries = {}
    for m in re.finditer(r'^\s*KERNEL_CMDLINE\[([^\]]+)\]\s*(\+?=)\s*"([^"]*)"', text, re.M):
        name, op, value = m.groups()
        entries[name] = (entries.get(name, '') + ' ' + value) if op == '+=' else value
    return entries


def status():
    m = method()
    want = wanted()
    running = cmdline_args(read('/proc/cmdline'))
    conf_cmd = configured_cmdline(m)
    mp = modprobe_options()
    items = []
    gtt_now = running.get('ttm.pages_limit') or read('/sys/module/ttm/parameters/pages_limit').strip()
    gtt_conf = conf_cmd.get('ttm.pages_limit', '')
    items.append({'key': 'ttm.pages_limit', 'label': 'GTT 14 GiB', 'want': GTT_PAGES,
                  'active': gtt_now, 'configured': gtt_conf, 'where': 'kernel command line'})
    for k, v in want.items():
        conf, where = '', ''
        if k in mp:
            conf, where = mp[k]
        if 'amdgpu.' + k in conf_cmd:
            conf, where = conf_cmd['amdgpu.' + k], 'kernel command line'
        items.append({'key': 'amdgpu.' + k, 'label': f'amdgpu {k}={v}', 'want': v,
                      'active': active(k), 'configured': conf, 'where': where})
    for it in items:
        it['ok'] = it['active'] == it['want']
        it['pending'] = not it['ok'] and it['configured'] == it['want']
        it['conflict'] = bool(it['configured']) and it['configured'] != it['want']
    vram = 0
    for f in glob.glob(str(R('/sys/class/drm')) + '/card*/device/mem_info_vram_total'):
        try:
            vram = max(vram, int(Path(f).read_text()) >> 20)
        except (OSError, ValueError):
            pass
    governors = [g for g in ('cyan-skillfish-governor-smu', 'cyan-skillfish-governor', 'cyan-skillfish-governor-tt', 'oberon-governor')
                 if ROOT == Path('/') and subprocess.run(['systemctl', 'is-active', '--quiet', g + '.service']).returncode == 0]
    helper = R('/run/kyty-lodhw.sock').is_socket() if ROOT == Path('/') else False
    return {'method': m, 'os': os_release().get('PRETTY_NAME', 'unknown'), 'patched_amdgpu': patched_module(),
            'items': items, 'vram_mib': vram, 'governor': governors, 'helper_running': helper,
            'reboot_needed': any(i['pending'] for i in items), 'all_ok': all(i['ok'] for i in items)}


def state_file():
    return R('/var/lib/kyty-beta-system/prepare-system.json')


def load_state():
    try:
        return json.loads(state_file().read_text())
    except (OSError, ValueError):
        return {'added': []}


def save_state(st):
    if a.dry_run:
        return
    state_file().parent.mkdir(parents=True, exist_ok=True)
    tmp = state_file().with_suffix('.tmp')
    tmp.write_text(json.dumps(st, indent=2) + '\n')
    tmp.replace(state_file())


def write_file(path, text, backup_dir):
    dest = R(path)
    print(f'  write {path}')
    if a.dry_run:
        print('    ' + text.rstrip().replace('\n', '\n    '))
        return None
    saved = None
    if dest.exists():
        backup_dir.mkdir(parents=True, exist_ok=True)
        saved = backup_dir / str(path).lstrip('/').replace('/', '__')
        shutil.copy2(dest, saved)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.kyty-new')
    tmp.write_text(text)
    if dest.exists():
        shutil.copymode(dest, tmp)
    else:
        tmp.chmod(0o644)
    tmp.replace(dest)
    return str(saved) if saved else None


def readonly_off():
    """SteamOS: /etc is writable, but update-grub writes /boot and /efi: lift the read-only flag for it."""
    if shutil.which('steamos-readonly') and ROOT == Path('/') and not a.dry_run:
        r = subprocess.run(['steamos-readonly', 'status'], text=True, capture_output=True)
        if 'enabled' in r.stdout:
            subprocess.run(['steamos-readonly', 'disable'], check=True)
            return True
    return False


def readonly_on(was):
    if was:
        subprocess.run(['steamos-readonly', 'enable'], check=False)


def regenerate(m):
    if m == 'steamos':
        was = readonly_off()
        try:
            run('update-grub')
        finally:
            readonly_on(was)
    elif m == 'limine':
        tool = shutil.which('limine-update') or shutil.which('limine-mkinitcpio')
        if not tool and ROOT == Path('/') and not a.dry_run:
            raise RuntimeError('limine-update / limine-mkinitcpio not found: run your Limine entry update by hand')
        run(tool or 'limine-update')
    elif m == 'grub':
        if shutil.which('update-grub'):
            run('update-grub')
        else:
            target = '/boot/grub/grub.cfg' if R('/boot/grub').is_dir() else '/boot/grub2/grub.cfg'
            run('grub-mkconfig', '-o', target)


def apply():
    st0 = status()
    m = st0['method']
    if m == 'other':
        raise RuntimeError('Unknown bootloader/OS. Add these kernel arguments by hand, then reboot:\n  ttm.pages_limit=' + GTT_PAGES +
                           ' ' + ' '.join(f'amdgpu.{k}={v}' for k, v in wanted().items()))
    if os.geteuid() != 0 and not a.dry_run and ROOT == Path('/'):
        raise RuntimeError('Run as root (AppRun --prepare-system uses sudo)')
    missing = [i for i in st0['items'] if not i['configured'] and not i['ok']]
    for i in st0['items']:
        if i['conflict']:
            print(f"  kept: {i['key']}={i['configured']} is set by you in {i['where'] or 'the bootloader'} (Kyty wants {i['want']})")
    if not missing:
        print('Nothing to add: every setting is configured.' + (' Reboot to apply.' if st0['reboot_needed'] else ''))
        return
    st = load_state()
    backup = R('/var/lib/kyty-beta-system/backups') / ('prepare-' + time.strftime('%Y%m%d-%H%M%S'))
    amd = {i['key'].split('.', 1)[1]: i['want'] for i in missing if i['key'].startswith('amdgpu.')}
    gtt = any(i['key'] == 'ttm.pages_limit' for i in missing)
    record = {'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'method': m, 'files': {}, 'kargs': []}

    if m == 'ostree':
        args = (['ttm.pages_limit=' + GTT_PAGES] if gtt else []) + [f'amdgpu.{k}={v}' for k, v in amd.items()]
        run('rpm-ostree', 'kargs', *[f'--append-if-missing={x}' for x in args])
        record['kargs'] = args
    elif m == 'steamos':
        if amd:
            text = MARK + ': KFD queues for the native backend (stock amdgpu values).\noptions amdgpu ' + \
                   ' '.join(f'{k}={v}' for k, v in amd.items()) + '\n'
            record['files']['/etc/modprobe.d/kytyps5-native.conf'] = write_file('/etc/modprobe.d/kytyps5-native.conf', text, backup)
        if gtt:
            text = MARK + ': 14 GiB GTT for PS5 games.\nGRUB_CMDLINE_LINUX_DEFAULT="${GRUB_CMDLINE_LINUX_DEFAULT:-} ttm.pages_limit=' + GTT_PAGES + '"\n'
            record['files']['/etc/default/grub.d/kytyps5-native.cfg'] = write_file('/etc/default/grub.d/kytyps5-native.cfg', text, backup)
            regenerate(m)
    elif m == 'limine':
        text = read('/etc/default/limine')
        args = (['ttm.pages_limit=' + GTT_PAGES] if gtt else []) + [f'amdgpu.{k}={v}' for k, v in amd.items()]
        lines = []
        for name, value in limine_entries(text).items():
            have = cmdline_args(value)
            add = [x for x in args if x.split('=', 1)[0] not in have]
            if add:
                lines.append(f'KERNEL_CMDLINE[{name}]+=" {" ".join(add)}"')
        if not limine_entries(text):
            lines.append(f'KERNEL_CMDLINE[default]+=" {" ".join(args)}"')
        if lines:
            new = text.rstrip('\n') + '\n\n' + MARK + ' (remove with: AppRun --prepare-system --revert)\n' + '\n'.join(lines) + '\n'
            record['files']['/etc/default/limine'] = write_file('/etc/default/limine', new, backup)
            record['limine_lines'] = lines
            regenerate(m)
    elif m == 'grub':
        args = (['ttm.pages_limit=' + GTT_PAGES] if gtt else []) + [f'amdgpu.{k}={v}' for k, v in amd.items()]
        text = read('/etc/default/grub')
        line = 'GRUB_CMDLINE_LINUX_DEFAULT="${GRUB_CMDLINE_LINUX_DEFAULT} ' + ' '.join(args) + '"'
        new = text.rstrip('\n') + '\n\n' + MARK + ' (remove with: AppRun --prepare-system --revert)\n' + line + '\n'
        record['files']['/etc/default/grub'] = write_file('/etc/default/grub', new, backup)
        record['grub_line'] = line
        regenerate(m)
    st['added'].append(record)
    save_state(st)
    print('Done. Reboot for the new settings to take effect.' if not a.dry_run else 'Dry run: nothing was written.')


def revert():
    st = load_state()
    if not st['added']:
        print('Nothing to revert: prepare-system has not changed this system.')
        return
    if os.geteuid() != 0 and not a.dry_run and ROOT == Path('/'):
        raise RuntimeError('Run as root (AppRun --prepare-system --revert uses sudo)')
    regen = set()
    for rec in reversed(st['added']):
        m = rec['method']
        if rec.get('kargs'):
            run('rpm-ostree', 'kargs', *[f'--delete-if-present={x}' for x in rec['kargs']])
        for path, saved in rec['files'].items():
            dest = R(path)
            if path in ('/etc/default/limine', '/etc/default/grub'):
                # Remove only our marked block; keep anything the user changed since.
                text = read(path)
                ours = rec.get('limine_lines', []) + ([rec['grub_line']] if rec.get('grub_line') else [])
                kept = [l for l in text.splitlines() if l not in ours and not l.startswith(MARK)]
                print(f'  remove KytyPS5 lines from {path}')
                if not a.dry_run:
                    dest.write_text('\n'.join(kept).rstrip('\n') + '\n')
                regen.add(m)
            else:
                print(f'  {"restore" if saved else "remove"} {path}')
                if not a.dry_run:
                    if saved and Path(saved).exists():
                        shutil.copy2(saved, dest)
                    else:
                        dest.unlink(missing_ok=True)
                if path.startswith('/etc/default/grub.d'):
                    regen.add(m)
    for m in regen:
        regenerate(m)
    if not a.dry_run:
        st['reverted'] = st.get('reverted', []) + st['added']
        st['added'] = []
        save_state(st)
    print('Reverted. Reboot to return to the previous settings.' if not a.dry_run else 'Dry run: nothing was written.')


def print_status(s):
    print(f"System: {s['os']}  (method: {s['method']}; amdgpu: {'patched BC-250' if s['patched_amdgpu'] else 'stock'})")
    for i in s['items']:
        state = 'OK' if i['ok'] else ('after reboot' if i['pending'] else ('CONFLICT' if i['conflict'] else 'MISSING'))
        print(f"  {state:12} {i['label']:34} active={i['active'] or '-'} configured={i['configured'] or '-'}")
    print(f"  {'OK' if s['vram_mib'] == 512 else 'CHECK':12} {'UMA/VRAM 512 MiB (BIOS)':34} {s['vram_mib']} MiB")
    print(f"  {'OK' if s['governor'] else 'RECOMMENDED':12} {'GPU clock governor':34} {', '.join(s['governor']) or 'none running'}")
    print(f"  {'OK' if s['helper_running'] else 'MISSING':12} {'kyty-lodhw GPU helper':34}")
    if not s['all_ok']:
        print('Run  AppRun --prepare-system  to add the missing settings.' if not s['reboot_needed'] else 'Reboot to apply the configured settings.')


def main():
    if a.status:
        s = status()
        print(json.dumps(s, indent=2) if a.json else '', end='')
        if not a.json:
            print_status(s)
        return
    if a.apply:
        apply()
    elif a.revert:
        revert()


if __name__ == '__main__':
    try:
        main()
    except RuntimeError as e:
        sys.exit('prepare-system: ' + str(e))
