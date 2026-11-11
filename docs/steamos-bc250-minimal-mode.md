# SteamOS BC-250: original PS5 Minimal Mode recovery

Observed on 2026-11-11, local timezone America/Los_Angeles.
Package: KytyPS5-BC250-2026.11.11-beta2-x86_64.
This report covers the original Minimal Mode only.

## Verified sequence

| Local time | Evidence |
| --- | --- |
| 04:12:53 | systemd-userdbd.service deactivated during isolation. pam_systemd failed to get the user record; ps5-minimal-session.service exited 224/PAM. |
| 04:18 | 10-keep-userdbd.conf installed (file timestamp). |
| 04:20:56 and 04:29:49 | labwc ran, but reported EGL_EXT_platform_base unsupported and could not create its renderer. The first fix had allowed startup to progress beyond PAM. |
| 04:44 | Installed labwc wrapper and egl-compat directory timestamps record the library fix. |
| 04:48 onward | Original Minimal Mode sessions started successfully. |
| 05:17:08 | labwc logged Mesa Project EGL and the hardware AMD BC-250 radeonsi/gfx1013 renderer. |

Journal timestamps establish the failure/success sequence. File timestamps establish when the present files were last written; they alone do not prove every action taken during troubleshooting. A systemd “Started” message for this Type=simple service is not by itself proof that the compositor became ready.

## Failure 1: user database unavailable during PAM session setup

Installed correction:

`/etc/systemd/system/ps5-minimal-session.service.d/10-keep-userdbd.conf`

```ini
[Unit]
Requires=systemd-userdbd.service
After=systemd-userdbd.service
```

The original session is isolated through ps5-minimal.target. Services outside the target's dependency closure can be stopped. The journal records userdbd stopping at precisely the failed PAM transition. PAMName=ps5-minimal opens a PAM session for deck before ExecStart. The local PAM policy requires pam_systemd.so with class=user-light, type=wayland and desktop=ps5-minimal. That module's user-record lookup failed while userdbd was unavailable.

This was a session setup failure, not evidence of an invalid password or missing deck account. NSS has passwd: files systemd and group: files [SUCCESS=merge] systemd; resolving a local passwd entry alone does not establish that pam_systemd can obtain the complete user record through its own lookup path.

Requires pulls userdbd into the session's dependency closure so isolation retains it; After orders session startup behind it. The unit itself requires its socket. The correction keeps the existing user-light session design, which avoids starting the full systemd user manager and desktop.

## Failure 2: mixed bundled Wayland and host Mesa ABI

Installed correction:

`/var/lib/kyty-beta-system/session-runtime/egl-compat/libwayland-client.so.0`

is a symlink to:

`/usr/lib/libwayland-client.so.0`

The wrapper at `/var/lib/kyty-beta-system/session-runtime/labwc` ends with:

```bash
exec /lib64/ld-linux-x86-64.so.2 \
  --library-path "$base/egl-compat:$base/lib" \
  "$base/bin/${0##*/}" "$@"
```

The package deliberately uses host EGL/Mesa but bundles other compositor libraries. The original loader search path preferred its older libwayland-client.so.0. SteamOS's newer Mesa EGL implementation needs wl_fixes_interface, which the bundled client does not export. The installed host client exports it. Selecting the older client prevented the host EGL implementation from loading correctly; the resulting extension/renderer messages were downstream symptoms.

A read-only reproduction during this investigation ran the same ctypes EGL client-extension query through the dynamic loader twice. With only session-runtime/lib, eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) returned None. With egl-compat first, it returned the platform extension list, including EGL_EXT_platform_base. Both calls reported EGL_SUCCESS, illustrating why checking eglGetError alone would miss this failure. Hardware EGL rendering is separately established by the successful labwc journal.

The compatibility directory changes library lookup only for this labwc process. The loader's --library-path argument does not export LD_LIBRARY_PATH into Kyty or Xwayland. The host library remains owned and updated by SteamOS.

## Essential files and artifacts

Essential: the userdbd dependency and ordering, host client symlink, and labwc wrapper search order.

Troubleshooting artifacts: labwc.before-egl-fix; Downloads/kyty-minimal-userdb-fix.sh; Downloads/kyty-minimal-egl-fix.sh; Downloads/kyty-minimal-egl-fix-v2.sh; Downloads/kyty-egl-client-probe.py. These are not runtime dependencies. Keep the backup until a packaged upgrade has been validated.

The original Beta 2 install-manifest.json did not track the manually added drop-in or compatibility symlink. The current wrapper path is tracked, but a reinstall from Beta 2 can overwrite its manual correction. An uninstall can leave the untracked drop-in and symlink behind.

## Installer integration completed in source after Beta 2

The accompanying beta/packaging/install-system.py change generates the working drop-in when the host ships systemd-userdbd.service. On hosts lacking that unit it leaves the dependency absent, rather than introducing an unavailable required service.

For bundled labwc it locates the host client in standard x86-64 distro directories, generates the compatibility symlink, and prepends egl-compat to that wrapper's library path. Missing host client is an installation error. Host-provided labwc remains managed by the host.

The symlink and drop-in participate in the install manifest and backup/restore lifecycle. Generated-file output supports links in dry runs and atomic link replacement in real installs. Backup restoration preserves an original symlink. This retains the packaged Wayland fallback libraries while selecting the host client for the compositor that loads host Mesa.

This integration writes only Kyty-owned files under /var/lib/kyty-beta-system and /etc. It requires no Mesa replacement, AMDGPU change, kernel modification, read-only root unlock, KDE dependency or Steam Gaming Mode dependency. It adds no helper process beyond retaining userdbd for PAM. No new RAM measurement was made; compositor/session success does not establish a particular memory footprint.

Validation performed: Python compilation and installer dry-run generation against the actual Beta 2 payload; inspected generated drop-in, link target and wrapper. Read-only ELF checks confirmed wl_fixes_interface exists only in the host client. The old/new library-path EGL query reproduced the failure and recovery. A real install, upgrade/uninstall lifecycle on a disposable host, and fresh installation/session transition remain release validation requirements. The working installation was not modified.

## Verification commands

Run from Desktop Mode; these inspect state without switching sessions:

```sh
systemctl cat ps5-minimal-session.service
systemctl show ps5-minimal-session.service -p Requires -p After
cat /etc/pam.d/ps5-minimal
readlink /var/lib/kyty-beta-system/session-runtime/egl-compat/libwayland-client.so.0
cat /var/lib/kyty-beta-system/session-runtime/labwc
nm -D /usr/lib/libwayland-client.so.0 | rg wl_fixes_interface
nm -D /var/lib/kyty-beta-system/session-runtime/lib/libwayland-client.so.0 | rg wl_fixes_interface
journalctl --list-boots
journalctl -b -1 -u ps5-minimal-session.service --no-pager
journalctl -b --no-pager | rg 'ps5-minimal|EGL vendor|GL renderer|EGL_EXT_platform_base'
pkexec /var/lib/kyty-beta-system/session/ps5-minimal-control check
```

The second nm command is expected to find no symbol. Full session verification requires a deliberate transition from a saved desktop: confirm UI readiness, hardware renderer, functioning input/audio, return to desktop, and memory usage. Do not use only systemctl is-active as the success criterion.

## Separate game failure observed

Only Ghost of Yōtei (PPSA26344) has launch logs in the inspected log directory. These do not establish the status of other games.

At 03:30 the log ended with GPU helper unavailable and no title GPU work submitted. That earlier problem is resolved in later logs: kyty-lodhw is active, VMID 1 is primed and registers are verified.

The 04:09 and 05:17 logs have the same later failure: frame 4 refuses opcode 0x22 at dword 1036 after 207 packets, then chunk 4 of 8 is dropped. Queues 0x30 and 0x38 subsequently wait on labels that remain zero. The 04:09 run predates the successful original Minimal Mode launch; the 05:17 run occurs in it. This supports a backend translation problem independent of the compositor startup failures.

Source identifies 0x22 as COND_EXEC. Both source and binary contain support, and the log explicitly reports COND_EXEC enabled. “Unknown packet” is a generic refusal message in nativeShadow.cpp; it does not prove that this opcode is unimplemented or that the binary is stale. The translator also refuses unsupported forms, nested ranges, invalid counts, CPU-side dependencies, and operations it cannot preserve within a conditional range. Existing logs omit the exact refusal reason and packet contents, so they cannot select a safe behavioral patch.

The latest game log reports GTT 14336 MiB and working graphics initialization. It also reports sched_policy=0, vm_update_mode=-1, cwsr_enable=1, plus missing Vulkan fragmentShaderBarycentric. Those are additional compatibility concerns, but none establishes the cause of this specific COND_EXEC rejection. Changing kernel parameters is not part of the Minimal Mode fix.

Next capture: reproduce with native audit logging enabled after the launcher's environment filtering, record the refusal reason and conditional packet/range, and compare whole-buffer translation with chunked translation. The upstream selector removes inherited KYTY_* variables; merely setting KYTY_NATIVE_GPU_AUDIT before AppRun will not enable this capture. Use a reviewable diagnostic launch path and the same runtime/save working directory. Do not skip opcode 0x22 or fabricate satisfied queue labels: either would alter game synchronization without resolving its semantics.

No game runtime fix is claimed by this report.
