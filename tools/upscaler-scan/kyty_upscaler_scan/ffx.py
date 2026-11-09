"""AMD FidelityFX (statically linked, PS5 AGC backend) route discovery.

Unlike PSML, FidelityFX is compiled into the game, so the runtime hook needs code addresses: where the backend's
job executor calls AgcCbDispatch (Kyty's HLE import sees that return address), the executor's stack slots for the
current job and backend, the FSR3 dispatcher that runs the executor and its stack slot for the dispatch
description, and the FSR3 shader headers. Everything here is DERIVED from the executable: the SDK's own pass-name
records (followed through R_X86_64_RELATIVE relocations), the SDK's diagnostic strings, the AgcCbDispatch import, and
the SDK's job-structure layout as it appears in the executor's instructions. A derivation that does not hold up
returns a candidate with the failed checks, never a guess.

Adapter `ffx_agc_fsr3upscaler_v1` is the FSR 3.1 upscaler on the FidelityFX PS5 backend whose structure layout was
established on Ghost of Yotei 02.002 (YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md). Its layout constants are checked here
in each game's code before the adapter is named; addresses differ per game and are not copied from Yotei.
"""
from __future__ import annotations

import re

from .dataflow import value_of
from .x86 import canon

COMPONENT = re.compile(rb'ffx_(fsr3upscaler|fsr3|fsr2|fsr1|frameinterpolation|opticalflow|cas|spd|lpm|dof|'
                       rb'blur|vrs|cacao|sssr|denoiser|classifier|brixelizer[a-z]*|parallelsort|lens)_[a-z0-9_]*?_pass')
HEADER_MAGIC = 0x34333231   # "1234": the PS5 backend's shader header magic (shader blob +0)

# SDK layout of the PS5 backend's scheduled compute job (stride 0x9a38) and its pipeline, as used by the executor.
ADAPTER_FSR3_V1 = {
    'adapter': 'ffx_agc_fsr3upscaler_v1',
    'job_stride': 0x9a38,
    'job_constants': (0x9a38, 0x9408, 0x8f04, 0x8ccc),       # job stride, UAV handles, SRV handles, CB records
    'job_pipeline': 0xa0, 'job_uav_count': 0xa8, 'job_srv_count': 0xac,
    'backend_resource_table': 0x30,
    # Internal dispatch description (RSI of the FSR3 dispatcher): color, depth, motion, exposure, reactive,
    # composition, output, jitter, mv scale, render size, output size, frame ms, pre-exposure, reset.
    'description': {'color': 0x8, 'depth': 0xb8, 'motion_vectors': 0x168, 'exposure': 0x218, 'reactive': 0x2c8,
                    'transparency_composition': 0x378, 'output': 0x638, 'jitter': 0x6e8, 'motion_vector_scale': 0x6f0,
                    'render_size': 0x6f8, 'output_size': 0x700, 'frame_time_ms': 0x710, 'pre_exposure': 0x714,
                    'reset': 0x718},
    'description_required': ('color', 'depth', 'motion_vectors', 'output', 'jitter', 'motion_vector_scale',
                             'render_size', 'reset'),
    'diagnostics': ('color resource is null', 'depth resource is null'),
    'input_names': ('r_input_color_jittered', 'r_input_depth', 'r_input_motion_vectors', 'r_input_exposure',
                    'r_reactive_mask', 'r_transparency_and_composition_mask', 'rw_upscaled_output'),
    'passes': ('prepare_inputs', 'prepare_reactivity', 'luma_instability', 'accumulate'),
}
AGC_CB_DISPATCH = 'k3GhuSNmBLU'


def _utf16_rvas(img, text):
    needle = text.encode('utf-16-le')
    return img.find_bytes(needle + b'\0\0', code=False) or img.find_bytes(needle, code=False)


def find_components(img):
    comps: dict[str, dict] = {}
    for seg in img.data_segments():
        for m in COMPONENT.finditer(seg.data):
            rva = seg.vaddr + m.start()
            name = img.cstring(rva)
            comp = m.group(1).decode()
            c = comps.setdefault(comp, {'passes': {}, 'records': []})
            pass_name = name.split('::')[0]
            c['passes'].setdefault(pass_name, []).append(rva)
    # Shader records {header, code, reflection, name} reached through relocations of the name pointer.
    for comp, c in comps.items():
        for pass_name, rvas in c['passes'].items():
            for name_rva in rvas:
                for ptr in img.pointers_to(name_rva):
                    base = ptr - 0x18
                    try:
                        header, code, refl = img.u64(base), img.u64(base + 8), img.u64(base + 16)
                    except ValueError:
                        continue
                    if img.segment_at(header) is None:
                        continue
                    magic = img.u32(header)
                    c['records'].append({'record': base, 'header': header, 'code': code, 'reflection': refl,
                                         'pass': pass_name, 'magic_ok': magic == HEADER_MAGIC})
        c['records'].sort(key=lambda r: r['record'])
    return comps


def _effective_dispatch_calls(img, dis, plt):
    """(call site, return address) of calls whose return address AgcCbDispatch's HLE sees: direct calls of the
    PLT stub, and calls of thin inline thunks that tail-jump (jmp/jcc) into it."""
    out = set()
    for site in img.rel32_callers(plt, calls=True, jumps=False):
        i = dis.insn_at(site)
        if i is not None and i.mnem.startswith('call'):
            out.add((site, i.end))
    for site in img.rel32_callers(plt, calls=False, jumps=True):
        i = dis.insn_at(site)
        if i is None or not i.mnem.startswith('j'):
            continue
        f = dis.function(site)
        if f is None or f[1] - f[0] > 0x80:     # thunks only: a few instructions
            continue
        for c in img.rel32_callers(f[0], calls=True, jumps=False):
            ci = dis.insn_at(c)
            if ci is not None and ci.mnem.startswith('call'):
                out.add((c, ci.end))
    return sorted(out)


def _slot_scores(insns, wanted_disps):
    """For each [rbp+X] that is loaded into a register, how often that register is then used as a base with one of
    `wanted_disps` before being redefined (within a short window)."""
    scores: dict[int, int] = {}
    for k, i in enumerate(insns):
        if i.mnem != 'mov':
            continue
        m = i.mem(1)
        r = i.reg_operand(0)
        if m is None or r is None or m.base != 'rbp' or m.index is not None or i.mem(0) is not None:
            continue
        for j in insns[k + 1:k + 16]:
            jm = j.mem()
            if jm is not None and jm.base == r and jm.disp in wanted_disps:
                scores[m.disp] = scores.get(m.disp, 0) + 1
                break
            if j.reg_operand(0) == r and j.mem(0) is None and not j.mnem.startswith(('cmp', 'test')):
                break
    return scores


def _imm_and_disp_constants(insns):
    vals = set()
    for i in insns:
        for op in i.operands():
            for tok in re.findall(r'0x[0-9a-f]+', op):
                vals.add(int(tok, 16))
    return vals


def _description_slot(insns):
    """The [rbp+X] slot the dispatcher stores its incoming RSI (the dispatch description) in, and the registers
    holding RSI that are used with displacements."""
    slots, holders = [], set()
    for k, i in enumerate(insns):
        if i.mnem == 'mov' and i.mem(0) is not None:
            m = i.mem(0)
            src = i.reg_operand(1)
            if m.base == 'rbp' and src is not None:
                v = value_of(insns, k, src)
                if v.kind == 'arg' and v.arg == 'rsi':
                    slots.append((m.disp, i.addr))
        r = i.reg_operand(0)
        if i.mnem == 'mov' and r is not None and len(i.operands()) == 2 and canon(i.operands()[1]) == 'rsi' and k < 40:
            holders.add(r)
    return slots, holders | {'rsi'}


def _interface_delta(img, dis, executor, dispatcher_insns):
    """Resolve which indirect call in the dispatcher reaches the executor: the backend's interface initialiser
    stores the executor (and its siblings) at [rdi+S]; the dispatcher calls them as [reg+S+delta]."""
    stores = {}
    for site in img.rip_lea_refs(executor):
        f = dis.function(site)
        if f is None:
            continue
        insns = f[2]
        lea_regs = {}
        for i in insns:
            if i.mnem == 'lea' and i.mem(1) is not None and i.mem(1).base == 'rip':
                lea_regs[i.reg_operand(0)] = i.mem(1).rip_target
            elif i.mnem == 'mov' and i.mem(0) is not None and i.reg_operand(1) in lea_regs:
                m = i.mem(0)
                if m.base is not None and m.base != 'rip' and m.index is None:
                    stores[m.disp] = lea_regs[i.reg_operand(1)]
    exec_slots = [s for s, f in stores.items() if f == executor]
    if not exec_slots:
        return None, None
    calls = [(i, i.mem()) for i in dispatcher_insns if i.mnem.startswith('call') and i.mem() is not None
             and i.mem().base not in (None, 'rip')]
    count: dict[int, int] = {}
    for _i, m in calls:
        count[m.disp] = count.get(m.disp, 0) + 1
    # FidelityFX's FfxInterface declares fpExecuteGpuJobs right after fpScheduleGpuJob: a dispatcher schedules each
    # pass (many calls of one slot) and then executes them once through the next slot. The executor's call is the
    # slot called exactly once whose predecessor slot is the dispatcher's most frequent indirect call.
    best = None
    for s_exec in exec_slots:
        if s_exec - 8 not in stores:
            continue
        for _i, m in calls:
            delta = m.disp - s_exec
            if count.get(s_exec + delta) != 1:
                continue
            sched = count.get(s_exec - 8 + delta, 0)
            if sched >= 2 and (best is None or sched > best[0]):
                best = (sched, delta, s_exec)
    if best is None:
        return None, {'interface_slot': exec_slots[0], 'delta': None, 'matched_calls': 0, 'total_calls': len(calls)}
    sched, delta, s_exec = best
    sites = [i for i, m in calls if m.disp == s_exec + delta]
    return sites, {'interface_slot': s_exec, 'delta': delta, 'schedule_calls': sched, 'total_calls': len(calls),
                   'schedule_function_rva': stores[s_exec - 8]}


def derive_fsr3_adapter(img, dis, comp):
    A = ADAPTER_FSR3_V1
    checks, rt = [], {}

    def check(name, ok, detail=''):
        checks.append({'check': name, 'ok': bool(ok), **({'detail': detail} if detail else {})})
        return ok

    recs = [r for r in comp['records'] if r['magic_ok']]
    check('FSR3 upscaler shader records found through relocations', recs, f'{len(recs)} records')
    if recs:
        rt['shader_record_table'] = recs[0]['record']
        rt['shader_record_first_header'] = recs[0]['header']
        rt['shader_header_range'] = [min(r['header'] for r in recs), max(r['header'] for r in recs)]
        passes = {r['pass'].replace('ffx_fsr3upscaler_', '').replace('_pass', '') for r in recs}
        check('the four passes the boundary uses are present', all(p in passes for p in A['passes']),
              ', '.join(sorted(passes)))
    imp = img.imports.get(AGC_CB_DISPATCH)
    if not check('AgcCbDispatch imported', imp is not None and imp.plt_rvas):
        return {'adapter': A['adapter'], 'state': 'candidate', 'checks': checks}
    calls = []
    for plt in imp.plt_rvas:
        calls += _effective_dispatch_calls(img, dis, plt)
    by_fn: dict[int, list] = {}
    for site, ret in calls:
        f = dis.function(site)
        if f is not None:
            by_fn.setdefault(f[0], []).append((site, ret))
    # The backend executor: a function dispatching through AgcCbDispatch whose code carries the job layout.
    executor = None
    for start, sites in by_fn.items():
        insns = dis.function(sites[0][0])[2]
        consts = _imm_and_disp_constants(insns)
        if all(c in consts for c in A['job_constants']):
            if executor is not None:
                check('exactly one FidelityFX job executor', False, f'{executor[0]:#x} and {start:#x}')
                return {'adapter': A['adapter'], 'state': 'ambiguous', 'checks': checks, 'runtime': rt}
            executor = (start, sites, insns)
    if not check('FidelityFX backend job executor located by its job layout', executor is not None,
                 f'{len(by_fn)} functions call AgcCbDispatch'):
        return {'adapter': A['adapter'], 'state': 'candidate', 'checks': checks, 'runtime': rt}
    ex_start, ex_sites, ex_insns = executor
    rt['executor_rva'] = ex_start
    rt['executor_return_rvas'] = sorted(ret for _s, ret in ex_sites)
    job = _slot_scores(ex_insns, {A['job_pipeline'], A['job_uav_count'], A['job_srv_count']})
    backend = _slot_scores(ex_insns, {A['backend_resource_table']})
    job_slot = max(job, key=job.get) if job else None
    backend_cands = {k: v for k, v in backend.items() if k != job_slot}
    backend_slot = max(backend_cands, key=backend_cands.get) if backend_cands else None
    check('executor job slot found ([rbp+X] used with job +0xa0/+0xa8/+0xac)', job_slot is not None,
          f'{job_slot:#x} ({job.get(job_slot)} uses)' if job_slot is not None else '')
    check('executor backend slot found ([rbp+X] used with +0x30)', backend_slot is not None,
          f'{backend_slot:#x} ({backend_cands.get(backend_slot)} uses)' if backend_slot is not None else '')
    rt['executor_job_slot'] = job_slot
    rt['executor_backend_slot'] = backend_slot
    # The FSR3 dispatcher: references the SDK's FSR3 diagnostics, keeps RSI (the description) in a stack slot,
    # reads the description at the adapter's offsets and calls the executor through the backend interface.
    diag_fns: dict[int, set] = {}
    for text in A['diagnostics']:
        for s in _utf16_rvas(img, text):
            for site in img.rip_lea_refs(s):
                f = dis.function(site)
                if f is not None:
                    diag_fns.setdefault(f[0], set()).add(text)
    cands = [fs for fs, t in diag_fns.items() if len(t) == len(A['diagnostics'])]
    dispatcher = None
    for fs in cands:
        f = dis.function(fs)
        insns = f[2]
        slots, holders = _description_slot(insns)
        used = set()
        for i in insns:
            m = i.mem()
            if m is not None and m.base in holders and m.index is None:
                used.add(m.disp)
        layout_hits = [n for n, off in A['description'].items() if off in used]
        if slots and all(n in layout_hits for n in A['description_required']):
            dispatcher = (f, slots, layout_hits)
            break
    if not check('FSR3 dispatcher located (SDK diagnostics + description layout + stored description)',
                 dispatcher is not None, f'{len(cands)} candidate(s)'):
        return {'adapter': A['adapter'], 'state': 'candidate', 'checks': checks, 'runtime': rt}
    (d_start, d_end, d_insns), slots, layout_hits = dispatcher
    rt['dispatcher_rva'] = d_start
    rt['dispatcher_range'] = [d_start, d_end]
    rt['dispatcher_description_slot'] = slots[0][0]
    rt['description_store_rva'] = slots[0][1]
    rt['description_fields_seen'] = layout_hits
    sites, info = _interface_delta(img, dis, ex_start, d_insns)
    if sites is not None and len(sites) == 1:
        rt['dispatcher_return_rva'] = sites[0].end
        rt['executor_call'] = {'rva': sites[0].addr, **info}
    detail = ''
    if info and info.get('delta') is not None:
        detail = (f"interface slot {info['interface_slot']:#x} called as +{info['interface_slot'] + info['delta']:#x}"
                  f" after {info['schedule_calls']} schedule calls")
    check('executor call in the dispatcher resolved through the backend interface table',
          sites is not None and len(sites) == 1, detail)
    names = {n: bool(_utf16_rvas(img, n)) for n in A['input_names']}
    check('named FSR3 input/output bindings present (UTF-16 name tables)', all(names.values()),
          ', '.join(n for n, ok in names.items() if not ok))
    ok = all(c['ok'] for c in checks)
    return {'adapter': A['adapter'], 'state': 'static_layout_verified' if ok else 'static_partial',
            'checks': checks, 'runtime': rt}


def analyse(img, dis):
    comps = find_components(img)
    routes = []
    summary = {k: {'passes': sorted(v['passes']), 'records': len(v['records'])} for k, v in comps.items()}
    if 'fsr3upscaler' in comps:
        r = derive_fsr3_adapter(img, dis, comps['fsr3upscaler'])
        r.update({'family': 'ffx_static', 'component': 'fsr3upscaler', 'runtime_hook': 'agc_cb_dispatch_capture',
                  'mode_requirement': 'base'})
        routes.append(r)
    for other in ('fsr2', 'fsr3'):
        if other in comps:
            routes.append({'family': 'ffx_static', 'component': other, 'adapter': None, 'state': 'candidate',
                           'mode_requirement': 'base', 'runtime_hook': None,
                           'checks': [{'check': f'adapter for FidelityFX {other}', 'ok': False,
                                       'detail': 'interface identified; no runtime adapter for this component yet'}]})
    return {'components': summary, 'routes': routes,
            'frame_interpolation_in_game': 'frameinterpolation' in comps}
