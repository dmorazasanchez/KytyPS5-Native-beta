#!/usr/bin/env python3
"""Check actual saved FG output, including HUD-free alignment and motion between adjacent real frames.

Usage: python verify_capture.py <dump directory>
Samples 10-bit pixels before any PNG conversion; phase correlation excludes the HUD and followed character.
"""
import json
import sys
from pathlib import Path

import numpy as np


def rgb10(a):
    return np.stack([((a >> s) & 1023).astype(np.int16) for s in (0, 10, 20)], -1)


def shift(a, b):
    h, w = a.shape[:2]
    roi = np.s_[int(h * .12):int(h * .58), int(w * .12):int(w * .88)]
    a, b = a[roi].mean(-1), b[roi].mean(-1)
    a, b = a - a.mean(), b - b.mean()
    r = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    r /= np.abs(r) + 1e-9
    c = np.fft.ifft2(r).real
    y, x = np.unravel_index(np.argmax(c), c.shape)
    h, w = c.shape
    return [int(-4 * (x if x <= w // 2 else x - w)),
            int(-4 * (y if y <= h // 2 else y - h)), round(float(c[y, x]), 4)]


def main():
    directory = Path(sys.argv[1])
    # Recreating the generator (e.g. an upscaler switch) restarts file numbering. Only the latest metadata for a
    # filename describes the bytes still on disk.
    latest = {}
    for line in (directory / 'fg-dump.jsonl').read_text().splitlines():
        if line.strip():
            e = json.loads(line)
            latest[e['n']] = e
    entries = sorted(latest.values(), key=lambda e: e['seq'])
    rows, previous = [], None
    for e in entries:
        raw = np.memmap(directory / f"fg-{e['n']:03d}.bin", dtype=np.uint32, mode='r')
        n, w, h = e['w'] * e['h'], e['w'], e['h']

        def plane(i):
            off = e['off'][i] // 4
            return rgb10(raw[off:off + n].reshape(h, w)[::4, ::4])

        real, generated = plane(0), plane(1)
        d = np.abs(real - generated).max(-1)
        row = {'dump': e['n'], 'time_s': e['t'], 'seq': e['seq'], 'want': e['want'],
               'mv_scale': e['mv_scale'], 'real_generated_identical_pct': round(float((d == 0).mean()) * 100, 3)}
        if e.get('hudless'):
            hudless = plane(4)
            d = np.abs(real - hudless).max(-1)
            sh, sw = d.shape
            center = d[sh // 4:3 * sh // 4, sw // 4:3 * sw // 4]
            row['hudless_center_within_one_code_pct'] = round(float((center <= 1).mean()) * 100, 3)
            row['hudless_all_within_one_code_pct'] = round(float((d <= 1).mean()) * 100, 3)
        if previous is not None and e['seq'] == previous[0]['seq'] + 1:
            row['background_shift_prev_to_real_px'] = shift(previous[1], real)
            row['background_shift_prev_to_generated_px'] = shift(previous[1], generated)
            row['background_shift_generated_to_real_px'] = shift(generated, real)
        previous = e, real
        rows.append(row)
    print(json.dumps({'directory': str(directory), 'frames': rows}, indent=2))


if __name__ == '__main__':
    main()
