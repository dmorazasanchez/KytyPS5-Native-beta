#!/usr/bin/env python3
"""Apply a game's HDR setting to the running PS5 Labwc session."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def write_config(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as file:
        file.write(data)
    os.replace(file.name, path)


def apply_outputs(compositor, randr):
    # Reload the config first. Querying the outputs also lets Labwc process the
    # reload before we ask it to reapply the enabled heads' current settings.
    subprocess.run([compositor, '--reconfigure'], check=True, timeout=3)
    outputs = json.loads(subprocess.check_output([randr, '--json'], timeout=3))
    output = next((head['name'] for head in outputs if head['enabled']), None)
    if output is not None:
        subprocess.run([randr, '--output', output, '--on'], check=True, timeout=5)


def set_hdr(enabled):
    path = Path(os.environ['KYTYPS5_LABWC_CONFIG'])
    compositor = os.environ['KYTYPS5_LABWC']
    randr = Path(__file__).resolve().parent / 'system/session-runtime/bin/wlr-randr'
    original = path.read_bytes()
    config = ET.fromstring(original, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
    core = config.find('core')
    if core is None:
        core = ET.SubElement(config, 'core')
    hdr = core.find('hdr')
    desired = 'yes' if enabled else 'no'
    if hdr is not None and hdr.text == desired:
        return
    if hdr is None:
        hdr = ET.SubElement(core, 'hdr')
    hdr.text = desired
    write_config(path, ET.tostring(config, encoding='utf-8', xml_declaration=True))
    try:
        apply_outputs(compositor, randr)
    except (OSError, ValueError, subprocess.SubprocessError):
        write_config(path, original)
        apply_outputs(compositor, randr)
        raise
    print(f'[Kyty launcher] Labwc display HDR {"allowed on supported outputs" if enabled else "off"}', file=sys.stderr)


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('on', 'off'):
        raise SystemExit('Usage: session-hdr.py on|off')
    if os.environ.get('KYTYPS5_LABWC_CONFIG') and os.environ.get('LABWC_PID'):
        try:
            set_hdr(sys.argv[1] == 'on')
        except (OSError, ValueError, ET.ParseError, subprocess.SubprocessError) as error:
            raise SystemExit(f'[Kyty launcher] Cannot change Labwc HDR: {error}')
