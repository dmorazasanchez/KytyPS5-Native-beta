"""PSML (libScePsml / libScePsml_mfsr2, Sony's PSSR interface) route discovery.

The runtime hook for this family is Kyty's own HLE implementation of the imported dispatch function: the game hands
its parameter block to the import, so no game code address is needed at run time. This module establishes offline
WHICH ABI revision the game uses (from the import NID), and checks in the game's own instructions that it fills the
parameter block the way that revision is laid out (resource-pointer slots, temporal fields). Nothing here is a
hard-coded per-game address: the call chain and the block are discovered by dataflow from the import.
"""
from __future__ import annotations

from .dataflow import Value, block_stores, value_of
from .x86 import Disassembler

# ABI catalogue: keyed by dispatch NID. Offsets are bytes into the parameter block. `source` says where the layout
# was established; `verified_titles` lists executables whose stores were checked against it (by this scanner or by
# the earlier manual work it reproduces).
PSML_ABIS = {
    '94iBp3KvIuI': {
        'abi': 'psml_legacy_r2',
        'library': 'libScePsml',
        'param_arg': 'rdx',
        'block_bytes': 0x160,
        'slots': {'output': (0, 0x00), 'color': (1, 0x08), 'motion_vectors': (2, 0x10), 'aux_slot3': (3, 0x18),
                  'depth': (4, 0x20), 'exposure': (5, 0x28), 'aux_slot6': (6, 0x30)},
        'required': ('output', 'color', 'motion_vectors', 'depth'),
        'fields': {'projection': (0x6c, 64), 'near': (0xe8, 4), 'far': (0xec, 4), 'jitter': (0x100, 8),
                   'motion_vector_scale': (0x108, 8), 'reset': (0x11c, 1)},
        'discriminators': {'reset': 0x11c, 'jitter': 0x100},
        'source': "Demon's Souls 1.007 historical dispatch capture (pssrInputContract.h) + Codex static check",
    },
    'RUNLFro+qok': {
        'abi': 'psml_legacy_r1',
        'library': 'libScePsml',
        'param_arg': 'rdx',
        'block_bytes': 0x150,
        'slots': {'output': (0, 0x00), 'color': (1, 0x08), 'motion_vectors': (2, 0x10), 'aux_slot3': (3, 0x18),
                  'depth': (4, 0x20), 'exposure': (5, 0x28), 'aux_slot6': (6, 0x30)},
        'required': ('output', 'color', 'motion_vectors', 'depth'),
        'fields': {'projection': (0x6c, 64), 'near': (0xe8, 4), 'far': (0xec, 4), 'render_size': (0xf0, 8),
                   'jitter': (0xf8, 8), 'motion_vector_scale': (0x100, 8), 'reset': (0x114, 1)},
        'discriminators': {'reset': 0x114, 'jitter': 0xf8},
        'source': "older legacy normalizer (chenxiao fork libPsml.cpp, DeS 1.005) + Codex ASTRO BOT static check",
    },
    'lrJwpLjKXRc': {
        'abi': 'psml_mfsr2',
        'library': 'libScePsml_mfsr2',
        'param_arg': 'rdx',
        'block_bytes': 0x70,
        'slots': {'output': (0, 0x00), 'color': (1, 0x08), 'motion_vectors': (2, 0x10), 'depth': (3, 0x18),
                  'exposure': (4, 0x20)},
        'required': ('output', 'color', 'motion_vectors', 'depth'),
        'fields': {'pre_exposure': (0x30, 4), 'render_rect': (0x34, 16), 'jitter': (0x44, 8),
                   'motion_vector_scale': (0x4c, 8), 'motion_vector_offset': (0x54, 8), 'depth_range': (0x5c, 8),
                   'reset': (0x64, 1), 'option_flags': (0x68, 4), 'sharpness': (0x6c, 4)},
        'discriminators': {},
        'source': 'AnyPS5 reverse-engineered Api.hpp via the Stage 2 registry (no real dispatch captured yet)',
    },
}
# Other libScePsml imports, labelled for the report only.
PSML_ROLES = {
    '3WVD91e12ZQ': 'initialize', '+2KpvixvL6E': 'shared_requirements', 'eWoKNeB6V-k': 'create_shared_resources',
    'jEevBXmagOQ': 'release_shared_resources', 'ArakEpzsZo0': 'context_requirements',
    'gxv3i+MTEzU': 'create_context', 'fccGInHrj8A': 'create_context', 'JaLBe0P3jSU': 'release_context',
    'AHalTX9wFZY': 'dispatch_size', 'GHna9-DvnUk': 'mipmap_bias', 'GJY0MvuTcs8': 'request_capture',
    'LXq+6mIxpCw': 'validate_object', 'FSGaTQze0UY': 'validate_object',
}

MAX_CHAIN = 4


def _confirmed_calls(img, dis, target):
    """Call sites of `target`, confirmed on instruction boundaries of their containing function."""
    out = []
    for site in img.rel32_callers(target, calls=True, jumps=False):
        f = dis.function(site)
        if f is None:
            continue
        insns = f[2]
        idx = next((k for k, i in enumerate(insns) if i.addr == site), None)
        if idx is not None and insns[idx].mnem.startswith('call'):
            out.append((f, idx))
    return out


def _trace_param(img, dis, target, reg, chain, results, seen):
    """Follow the parameter register from calls of `target` up through wrappers until its value is defined."""
    for f, idx in _confirmed_calls(img, dis, target):
        start, end, insns = f
        key = (insns[idx].addr, reg)
        if key in seen:
            continue
        seen.add(key)
        v = value_of(insns, idx, reg)
        link = {'function_rva': start, 'call_rva': insns[idx].addr, 'callee_rva': target}
        if v.kind == 'arg' and len(chain) < MAX_CHAIN:
            # A wrapper passing its own incoming argument through: continue with its callers.
            _trace_param(img, dis, start, v.arg, chain + [dict(link, passes=v.arg)], results, seen)
        else:
            results.append((chain + [link], f, idx, v))


def _describe_block(v: Value, function_rva):
    if v.kind != 'address' or v.mem is None:
        return None
    m = v.mem
    if m.base == 'rip':
        return {'kind': 'global', 'rva': m.rip_target}
    if m.base == 'rbp' and m.index is None:
        return {'kind': 'stack', 'frame_register': 'rbp', 'frame_offset': m.disp, 'function_rva': function_rva}
    if m.base == 'rsp' and m.index is None:
        return {'kind': 'stack', 'frame_register': 'rsp', 'frame_offset': m.disp, 'function_rva': function_rva}
    return {'kind': 'register_relative', 'base': m.base, 'offset': m.disp}


def _covers(stores, off, n):
    return [s for s in stores if s[0] <= off and off + n <= s[0] + max(s[1], 1)]


def analyse_dispatch(img, dis: Disassembler, nid: str):
    abi = PSML_ABIS[nid]
    imp = img.imports[nid]
    route = {
        'family': 'psml', 'abi': abi['abi'], 'abi_source': abi['source'],
        'entry': {'kind': 'import', 'nid': nid, 'library': imp.library, 'module': imp.module,
                  'got_rva': imp.got_rva, 'plt_rvas': list(imp.plt_rvas)},
        'runtime_hook': 'kyty_hle_import',       # Kyty implements this import itself: the game passes the block to us
        'mode_requirement': 'ps5_pro',            # Sony's PSSR runs only on the PS5 Pro (trinity) path
        'callsites': [], 'checks': [], 'state': 'candidate',
    }
    if not imp.plt_rvas:
        route['checks'].append({'check': 'dispatch PLT stub found', 'ok': False})
        return route
    results = []
    for plt in imp.plt_rvas:
        _trace_param(img, dis, plt, abi['param_arg'], [], results, set())
    route['checks'].append({'check': 'game code calls the dispatch import (through wrappers)', 'ok': bool(results),
                            'detail': f'{len(results)} call path(s)'})
    best = None
    for chain, f, idx, v in results:
        start, end, insns = f
        block = _describe_block(v, start)
        site = {'chain': [{k: (hex(x) if isinstance(x, int) else x) for k, x in link.items()} for link in chain],
                'game_function_rva': hex(start), 'game_call_rva': hex(insns[idx].addr),
                'parameter_value': v.kind, 'parameter_block': None, 'resources': {}, 'fields': {}, 'checks': []}
        if v.kind == 'load' and v.mem is not None:
            site['parameter_block'] = {'kind': 'pointer_loaded', 'from': _mem_text(v.mem)}
        if block is not None:
            site['parameter_block'] = {k: (hex(x) if isinstance(x, int) and k in ('rva', 'function_rva') else x)
                                       for k, x in block.items()}
            if block['kind'] == 'stack':
                site['parameter_block']['frame_offset_hex'] = ('-' if block['frame_offset'] < 0 else '') + \
                    hex(abs(block['frame_offset']))
            stores = block_stores(insns, v, abi['block_bytes'] + 0x40, exclude_after=insns[idx].addr)
            score = _match_layout(abi, stores, site)
            if best is None or score > best[0]:
                best = (score, site)
        route['callsites'].append(site)
    if best is not None and best[0] >= 0:
        site = best[1]
        ok_required = all(site['resources'].get(r, {}).get('stored_from_register') for r in abi['required'])
        temporal = [n for n in ('jitter', 'motion_vector_scale', 'reset') if n in abi['fields']]
        ok_temporal = all(site['fields'].get(n, {}).get('stored') for n in temporal)
        route['primary_callsite'] = site['game_call_rva']
        route['checks'].append({'check': 'required resource slots written from registers (' +
                                ', '.join(abi['required']) + ')', 'ok': ok_required})
        route['checks'].append({'check': 'temporal fields written at this ABI\'s offsets', 'ok': ok_temporal})
        disc = site.get('discriminator_conflicts', [])
        route['checks'].append({'check': 'no stores at another revision\'s distinguishing offsets', 'ok': not disc,
                                'detail': ', '.join(disc)})
        if ok_required and ok_temporal and not disc:
            route['state'] = 'static_layout_verified'
        elif ok_required:
            route['state'] = 'static_partial'
    elif results:
        route['checks'].append({'check': 'parameter block address resolved statically', 'ok': False,
                                'detail': ', '.join(sorted({r[3].kind + (':' + r[3].why if r[3].why else '')
                                                           for r in results}))})
    return route


def _mem_text(m):
    s = m.base or ''
    if m.index:
        s += f'+{m.index}*{m.scale}'
    if m.disp:
        s += f'{m.disp:+#x}'
    return f'[{s}]'


def _match_layout(abi, stores, site):
    """Fill `site` with slot/field evidence; returns a score (-1: nothing recognisable)."""
    if not stores:
        return -1
    score = 0
    for role, (slot, off) in abi['slots'].items():
        hit = [s for s in stores if s[0] == off and s[1] == 8]
        wide = _covers(stores, off, 8)
        entry = {'slot': slot, 'parameter_offset': hex(off), 'store_rvas': [hex(s[2].addr) for s in hit or wide]}
        if hit:
            src = hit[-1][3]
            from_reg = '[' not in src and not src.lower().startswith('0x') and src not in ('0',)
            entry['stored_from_register'] = from_reg
            entry['source'] = src
            score += 2 if from_reg else 1
        elif wide:
            entry['stored_from_register'] = None  # inside a wider (vector) store: value unknown statically
            entry['source'] = 'vector store'
        else:
            entry['stored_from_register'] = False
        site['resources'][role] = entry
    for name, (off, n) in abi['fields'].items():
        overlap = [s for s in stores if s[0] < off + n and s[0] + max(s[1], 1) > off]
        covered = set()
        for s in overlap:
            covered.update(range(max(s[0], off), min(s[0] + max(s[1], 1), off + n)))
        full = len(covered) == n
        site['fields'][name] = {'parameter_offset': hex(off), 'bytes': n, 'stored': full,
                                'store_rvas': sorted({hex(s[2].addr) for s in overlap})}
        score += 1 if full else 0
    # Another revision's distinguishing offsets written as single small stores: a hint that the ABI differs.
    conflicts = []
    for other in PSML_ABIS.values():
        if other is abi:
            continue
        for name, off in other['discriminators'].items():
            mine = abi['fields'].get(name)
            if mine is None or mine[0] == off:
                continue
            n = abi['fields'][name][1]
            if any(s[0] == off and s[1] == n for s in stores) and not _covers(stores, mine[0], n):
                conflicts.append(f"{name} at {off:#x} ({other['abi']})")
    site['discriminator_conflicts'] = conflicts
    site['store_count'] = len(stores)
    return score


def analyse(img, dis):
    routes = []
    present = {nid: img.imports[nid] for nid in PSML_ABIS if nid in img.imports}
    other = {nid: PSML_ROLES[nid] for nid in PSML_ROLES if nid in img.imports}
    for nid in present:
        routes.append(analyse_dispatch(img, dis, nid))
    libs = sorted({imp.library for imp in img.imports.values() if imp.library.startswith('libScePsml')})
    unknown = sorted(nid for nid, imp in img.imports.items()
                     if imp.library.startswith('libScePsml') and nid not in PSML_ABIS and nid not in PSML_ROLES)
    return {'libraries': libs, 'other_imports': other, 'unknown_imports': unknown, 'routes': routes}
