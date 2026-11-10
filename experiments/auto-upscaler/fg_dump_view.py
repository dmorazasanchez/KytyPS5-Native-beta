#!/usr/bin/env python3
"""Frame generation dumps (KYTY_FRAMEGEN_DUMP) into PNGs and a motion check.

Each dump n holds the real frame, the generated frame shown just before it (between the previous real frame and this
one), and the depth and motion guides interpolation read for it. For consecutive dumps the check measures the global
image shift (phase correlation) prev real -> generated -> real, against the motion vectors' own median: a right frame
sits halfway; a frame built from another frame's guides, or with mis-scaled vectors, does not, and two strong
correlation peaks mean a double image.

usage: fg_dump_view.py <dump dir> [--png]
"""
import json
import sys
from pathlib import Path

import numpy as np

VK_B8G8R8A8 = 44
VK_R8G8B8A8 = 37
VK_A2B10G10R10 = 64


def colour(raw, w, h, fmt):
    a = np.frombuffer(raw, dtype=np.uint32, count=w * h).reshape(h, w)
    if fmt == VK_A2B10G10R10:
        r = (a & 0x3ff) >> 2
        g = ((a >> 10) & 0x3ff) >> 2
        b = ((a >> 20) & 0x3ff) >> 2
    else:
        r, g, b = a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff
        if fmt == VK_B8G8R8A8:
            r, b = b, r
    return np.stack([r, g, b], -1).astype(np.uint8)


def load(d, e):
    blob = (d / f"fg-{e['n']:03d}.bin").read_bytes()
    w, h, gw, gh = e['w'], e['h'], e['gw'], e['gh']
    off = e['off']
    real = colour(blob[off[0]:off[0] + w * h * 4], w, h, e['vk_format'])
    gen = colour(blob[off[1]:off[1] + w * h * 4], w, h, e['vk_format'])
    depth = np.frombuffer(blob, dtype=np.float32, count=gw * gh, offset=off[2]).reshape(gh, gw)
    mv = np.frombuffer(blob, dtype=np.float16, count=gw * gh * 2, offset=off[3]).reshape(gh, gw, 2).astype(np.float32)
    return real, gen, depth, mv


def load_hudless(d, e):
    """The HUD-less frame of dump e (A2B10G10R10), or None."""
    if not e.get('hudless'):
        return None
    blob = (d / f"fg-{e['n']:03d}.bin").read_bytes()
    w, h = e['w'], e['h']
    return colour(blob[e['off'][4]:e['off'][4] + w * h * 4], w, h, VK_A2B10G10R10)


def raw10(blob_part, w, h):
    a = np.frombuffer(blob_part, dtype=np.uint32, count=w * h).reshape(h, w)
    return np.stack([a & 0x3ff, (a >> 10) & 0x3ff, (a >> 20) & 0x3ff], -1).astype(np.int32)


def gray(img, step=4):
    g = img[::step, ::step].astype(np.float32).mean(-1)
    return g - g.mean()


def phase_peaks(a, b, step=4, k=3):
    """How far the content moved from a to b (display pixels, phase correlation): the k strongest (dx, dy, strength)."""
    A, B = np.fft.fft2(gray(a, step)), np.fft.fft2(gray(b, step))
    R = A * np.conj(B)
    R /= np.abs(R) + 1e-9
    c = np.real(np.fft.ifft2(R))
    h, w = c.shape
    peaks = []
    flat = c.copy()
    for _ in range(k):
        i = int(np.argmax(flat))
        y, x = divmod(i, w)
        v = float(flat[y, x])
        dy = y if y <= h // 2 else y - h
        dx = x if x <= w // 2 else x - w
        peaks.append((-dx * step, -dy * step, round(v, 3)))
        flat[max(0, y - 3):y + 4, max(0, x - 3):x + 4] = -1  # (suppress the neighbourhood)
    return peaks


def mv_png(mv, path):
    from PIL import Image
    m = np.hypot(mv[..., 0], mv[..., 1])
    s = np.percentile(m, 99) or 1.0
    img = np.zeros(mv.shape[:2] + (3,), np.uint8)
    img[..., 0] = np.clip(128 + 127 * mv[..., 0] / s, 0, 255)
    img[..., 1] = np.clip(128 + 127 * mv[..., 1] / s, 0, 255)
    img[..., 2] = np.clip(255 * m / s, 0, 255)
    Image.fromarray(img).save(path)


def main():
    d = Path(sys.argv[1])
    png = '--png' in sys.argv
    entries = [json.loads(l) for l in (d / 'fg-dump.jsonl').read_text().splitlines() if l.strip()]
    entries.sort(key=lambda e: e['n'])
    prev = None
    for e in entries:
        real, gen, depth, mv = load(d, e)
        # Vectors: guide pixels via mv_scale (FSR: previous = current + mv * scale), times the guide factor = display px.
        f = e['factor']
        px = mv * np.array(e['mv_scale'], np.float32) * f
        valid = np.isfinite(px).all(-1)
        med = -np.median(px[valid], axis=0) if valid.any() else np.zeros(2)  # current - previous, as the shifts below
        print(f"dump {e['n']}: t {e['t']:.2f}s seq {e['seq']} want {e['want']} reset {e['reset']} frame_ms {e['frame_ms']:.1f} "
              f"vectors' median motion (cur - prev) {med[0]:+.1f},{med[1]:+.1f} px; depth {np.nanmin(depth):.4g}..{np.nanmax(depth):.4g}")
        if prev is not None and e['n'] == prev[0]['n'] + 1:
            p_real = prev[1]
            print('   prev real -> real      ', phase_peaks(p_real, real))
            print('   prev real -> generated ', phase_peaks(p_real, gen))
            print('   generated -> real      ', phase_peaks(gen, real))
        if png:
            from PIL import Image
            Image.fromarray(real).save(d / f"fg-{e['n']:03d}-real.png")
            Image.fromarray(gen).save(d / f"fg-{e['n']:03d}-gen.png")
            mv_png(mv, d / f"fg-{e['n']:03d}-mv.png")
            dn = depth / (np.nanmax(depth) or 1)
            Image.fromarray((np.clip(dn, 0, 1) * 255).astype(np.uint8)).save(d / f"fg-{e['n']:03d}-depth.png")
        prev = (e, real)


if __name__ == '__main__':
    main()
