#!/usr/bin/python3
"""Install opt-in PS5 Minimal RAM audit without replacing the user's session."""
from pathlib import Path
import shutil
import os
import subprocess
import datetime

SRC=Path("/home/david/ps5-minimal-session-20261109/files/ps5-ram-audit")
DEST=Path("/usr/local/libexec/ps5-ram-audit")
SERVICE=Path("/etc/systemd/system/ps5-minimal-ram-audit.service")
TIMER=Path("/etc/systemd/system/ps5-minimal-ram-audit.timer")

service="""[Unit]
Description=Snapshot real RAM usage inside PS5 Minimal mode
DefaultDependencies=no
After=sysinit.target ps5-minimal-session.service
ConditionPathExists=/run/ps5-minimal/state.json
Conflicts=shutdown.target
Before=shutdown.target

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/ps5-ram-audit
TimeoutStartSec=20
"""
timer="""[Unit]
Description=Sample PS5 Minimal real RAM periodically for comparison with KDE
DefaultDependencies=no
PartOf=ps5-minimal.target
Conflicts=shutdown.target
Before=shutdown.target

[Timer]
OnActiveSec=20s
OnUnitInactiveSec=30s
AccuracySec=2s
Unit=ps5-minimal-ram-audit.service

[Install]
WantedBy=ps5-minimal.target
"""
for dest in (DEST,SERVICE,TIMER):
    if dest.exists():
        raise RuntimeError(f"{dest} already exists; aborting rather than overwriting")
assert SRC.is_file()
shutil.copy2(SRC,DEST)
os.chmod(DEST,0o755)
SERVICE.write_text(service)
TIMER.write_text(timer)
for p in (SERVICE,TIMER):
    os.chmod(p,0o644)
subprocess.run(["systemctl","daemon-reload"],check=True)
subprocess.run(["systemctl","enable","ps5-minimal-ram-audit.timer"],check=True)
print("Installed diagnostic (samples only when PS5 Minimal target is active).")
