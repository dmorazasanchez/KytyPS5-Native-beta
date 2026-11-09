#!/usr/bin/env python3
"""Find a title's temporal AA / upscaling pass in a KYTY_PASS_CAPTURE window (window-<n>.jsonl), with no names.

The structural signature of TAA: a pass reads its own previous output. With two history images that pass writes A
and reads B in one frame, then writes B and reads A in the next (ping-pong); captured per invocation that is two
image slots of the same shader, same format and size, whose addresses take the same two values in anti-phase.
For a draw, one side is a colour target. Around that pair the pass also reads the frame's colour (render size),
motion vectors (2 channels) and depth. Formats and sizes only RANK the roles; they never create a candidate.
"""
import argparse
import collections
import json
import sys

COLOR = {36: 'R11G11B10F', 71: 'RGBA16F', 56: 'RGBA8', 43: 'R10G11B11F', 44: 'RGB10A2', 50: 'RGB10A2', 65: 'RGBA16',
         77: 'RGBA32F', 13: 'R16F'}
MOTION = {29: 'RG16F', 23: 'RG16', 24: 'RG16S', 64: 'RG32F'}
DEPTH = {22: 'R32F', 20: 'R32U', 7: 'R16'}
RT_FMT = {12: 'RT 16_16_16_16?', 10: 'RT 8_8_8_8?', 7: 'RT 11_11_10?', 5: 'RT 16_16?'}  # CB_COLOR_INFO.FORMAT


def fmt_name(f):
    return COLOR.get(f) or MOTION.get(f) or DEPTH.get(f) or (f'rt{f - 1000}' if f >= 1000 else f'fmt{f}')


def load(path):
    shaders = collections.defaultdict(list)
    kinds = {}
    with open(path) as fh:
        for line in fh:
            p = json.loads(line)
            slots = {}
            for t in p['t']:
                if not (64 <= t['w'] <= 8192 and 64 <= t['h'] <= 8192) or t['type'] not in (9, 13):  # 2D / 2D array
                    continue
                slots[(t['at'], t['word'])] = (t['base'], t['fmt'], t['w'], t['h'])
            for r in p['rt']:
                slots[('rt', r['slot'])] = (r['base'], 1000 + r['fmt'], r['w'], r['h'])
            if p.get('z', {}).get('base', '0x0') != '0x0' and p['kind'] == 'draw':
                z = p['z']
                slots[('z', 0)] = (z['base'], 2000, z['w'], z['h'])
            shaders[p['shader']].append((p['seq'], p.get('flip', 0), slots))
            kinds[p['shader']] = (p['kind'], p['grid'], p['threads'])
    return shaders, kinds


def pingpong_pairs(invs):
    """(slot_a, slot_b, values, n): same format+size, same 2+ values, never equal in one invocation."""
    per_slot = collections.defaultdict(set)
    for _seq, _flip, s in invs:
        for k, v in s.items():
            per_slot[k].add(v[0])
    multi = [k for k, vals in per_slot.items() if 2 <= len(vals) <= 3]
    out = []
    for i, a in enumerate(multi):
        for b in multi[i + 1:]:
            pairs = [(s[a], s[b]) for _q, _f, s in invs if a in s and b in s]
            if len(pairs) < 3:
                continue
            fa, fb = pairs[0][0][1:], pairs[0][1][1:]
            same_shape = fa[1:] == fb[1:] and (fa[0] == fb[0] or a[0] == 'rt' or b[0] == 'rt')
            if not same_shape:
                continue
            va = {x[0][0] for x in pairs}
            vb = {x[1][0] for x in pairs}
            if va == vb and len(va) >= 2 and all(x[0][0] != x[1][0] for x in pairs):
                # both directions must occur (a really swaps): A->B and B->A
                dirs = {(x[0][0], x[1][0]) for x in pairs}
                if len(dirs) >= 2:
                    out.append((a, b, sorted(va), len(pairs), fa, fb))
    return out


def roles(invs, pair):
    """Rank the other slots of the pass for colour / motion / depth roles (formats and sizes rank, nothing more)."""
    a, b, vals, n, fa, fb = pair
    hist_w, hist_h = fa[1], fa[2]
    seen = collections.defaultdict(collections.Counter)
    shape = {}
    for _q, _f, s in invs:
        for k, v in s.items():
            if k in (a, b):
                continue
            seen[k][v[0]] += 1
            shape[k] = v[1:]
    cand = {'color': [], 'motion': [], 'depth': [], 'other': []}
    for k, cnt in seen.items():
        f, w, h = shape[k]
        stable = len(cnt) == 1
        info = {'slot': list(k), 'fmt': fmt_name(f), 'size': [w, h], 'distinct_values': len(cnt)}
        if f in MOTION:
            cand['motion'].append(info)
        elif f in DEPTH or f == 2000:
            cand['depth'].append(info)
        elif f in COLOR and w <= hist_w and h <= hist_h:
            cand['color'].append(info)
        else:
            cand['other'].append(info)
    return cand


def analyse(path, top=8):
    shaders, kinds = load(path)
    results = []
    for sh, invs in shaders.items():
        if len(invs) < 3:
            continue
        for pair in pingpong_pairs(invs):
            a, b, vals, n, fa, fb = pair
            is_color = fa[0] in COLOR or fa[0] >= 1000
            r = roles(invs, pair)
            score = (3 if is_color else 0) + (2 if r['motion'] else 0) + (1 if r['depth'] else 0) + (1 if r['color'] else 0) + \
                (1 if fa[1] * fa[2] >= 1280 * 720 else 0)
            results.append({'shader': sh, 'kind': kinds[sh][0], 'grid': kinds[sh][1], 'threads': kinds[sh][2],
                            'invocations': len(invs), 'history': {'slots': [list(a), list(b)], 'fmt': fmt_name(fa[0]),
                                                                   'size': [fa[1], fa[2]], 'values': vals, 'n': n},
                            'roles': r, 'score': score})
    results.sort(key=lambda x: (-x['score'], -x['history']['size'][0] * x['history']['size'][1]))
    return results[:top]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('windows', nargs='+')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    for w in a.windows:
        res = analyse(w, a.top)
        if a.json:
            print(json.dumps({'window': w, 'candidates': res}))
            continue
        print(f'== {w}: {len(res)} candidate(s)')
        for c in res:
            h = c['history']
            print(f"  score {c['score']} {c['kind']} {c['shader']} grid={c['grid']} thr={c['threads']} x{c['invocations']}: "
                  f"history {h['fmt']} {h['size'][0]}x{h['size'][1]} slots {h['slots']} n={h['n']}")
            for role in ('color', 'motion', 'depth'):
                for x in c['roles'][role][:4]:
                    print(f"      {role:6s} {x['slot']} {x['fmt']} {x['size'][0]}x{x['size'][1]} values={x['distinct_values']}")


if __name__ == '__main__':
    sys.exit(main())
