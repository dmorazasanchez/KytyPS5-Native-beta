"""The scanner must rediscover, with no per-game addresses, what earlier manual work verified for the installed games.

Ground truth comes from the earlier work's own files, not from this scanner:
  - Ghost of Yotei: the constants the working native upscaler hard-codes (seed commit's nativeUpscaler.cpp), which
    were verified live (YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md: 592 named bindings matched).
  - Demon's Souls 1.007 and ASTRO BOT 1.018: Codex's hand-checked identifier JSONs.
Tests skip when a game or a truth file is not installed. Run: python3 -I -m unittest discover -s tests
"""
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from kyty_upscaler_scan import ffx, game, psml, scan, selfelf, x86  # noqa: E402

DL = Path('/home/david/Downloads')
YOTEI = DL / 'Ghost of Yōtei Complete Edition/UP9000-PPSA26344_00-GHOST2SHIP000000-files'
DEMONS = DL / "Demons Souls (v01.007) (Deluxe Edition) – BP 4.xx – PPSA01341 – FPKG/EP9000-PPSA01341_00-DEMONSSOULS00000-files"
ASTRO = DL / '[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018/[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018.zar'
DEMONS_TRUTH = Path('/home/david/demons-upscaler-identifiers-20261108/DEMONS_1007_UPSCALER_IDENTIFIERS.json')
ASTRO_TRUTH = Path('/home/david/astro-upscaler-identifiers-20261108/ASTRO_BOT_1018_IDENTIFIERS.json')
SEED_COMMIT = '0631f62'
WORKTREE = HERE.parents[2]
BASE = scan.MODULE_BASE

_cache = {}


def analysed(path):
    if path not in _cache:
        g = game.load(path)
        img = selfelf.SelfImage(g.eboot)
        dis = x86.Disassembler(img)
        _cache[path] = (g, img, psml.analyse(img, dis), ffx.analyse(img, dis))
    return _cache[path]


def h(x):
    return int(x, 16) if isinstance(x, str) else x


def seed_upscaler_source():
    try:
        return subprocess.run(['git', '-C', str(WORKTREE), 'show',
                               f'{SEED_COMMIT}:src/graphics/native_gpu/nativeUpscaler.cpp'],
                              capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None


@unittest.skipUnless(YOTEI.is_dir(), 'Ghost of Yotei not installed')
class Yotei(unittest.TestCase):
    """The FSR3 hook constants of the working, live-verified Yotei integration, derived instead of hard-coded."""

    @classmethod
    def setUpClass(cls):
        cls.src = seed_upscaler_source()
        cls.g, cls.img, cls.psml, cls.ffx = analysed(YOTEI)
        cls.route = next(r for r in cls.ffx['routes'] if r.get('adapter') == 'ffx_agc_fsr3upscaler_v1')
        cls.rt = cls.route['runtime']

    def need_src(self):
        if self.src is None:
            self.skipTest('seed commit not available')

    def test_adapter_verified(self):
        failed = [c for c in self.route['checks'] if not c['ok']]
        self.assertEqual(failed, [])
        self.assertEqual(self.route['state'], 'static_layout_verified')

    def test_executor_return_pcs(self):
        self.need_src()
        m = re.search(r'if \(pc != (0x[0-9a-f]+)ULL && pc != (0x[0-9a-f]+)ULL\)', self.src)
        truth = {int(m.group(1), 16) - BASE, int(m.group(2), 16) - BASE}
        self.assertTrue(truth <= set(self.rt['executor_return_rvas']), (truth, self.rt['executor_return_rvas']))
        # The other derived return addresses are on the executor's internal (job type 0/1) paths; the runtime's
        # job-type check filters them. Every derived one is a call into the AgcCbDispatch thunk of the executor.
        self.assertTrue(all(self.rt['executor_rva'] < r < self.rt['executor_rva'] + 0x2000
                            for r in self.rt['executor_return_rvas']))

    def test_stack_slots(self):
        self.need_src()
        job = int(re.search(r'job = At<uint64_t>\(frame - (0x[0-9a-f]+)\)', self.src).group(1), 16)
        backend = int(re.search(r'backend = At<uint64_t>\(frame - (0x[0-9a-f]+)\)', self.src).group(1), 16)
        params = int(re.search(r'params = At<uint64_t>\(At<uint64_t>\(frame\) - (0x[0-9a-f]+)\)', self.src).group(1), 16)
        self.assertEqual(self.rt['executor_job_slot'], -job)
        self.assertEqual(self.rt['executor_backend_slot'], -backend)
        self.assertEqual(self.rt['dispatcher_description_slot'], -params)
        self.assertEqual(self.rt['description_store_rva'], 0x15434)  # the store Codex's findings cite

    def test_dispatcher_return(self):
        self.need_src()
        ret = int(re.search(r'At<uint64_t>\(frame \+ 8\) != (0x[0-9a-f]+)ULL', self.src).group(1), 16)
        self.assertEqual(self.rt['dispatcher_return_rva'], ret - BASE)
        lo, hi = self.rt['dispatcher_range']
        self.assertTrue(lo <= ret - BASE < hi)
        self.assertEqual(self.rt['dispatcher_rva'], 0x14fd0)  # YOTEI_FSR3_HOST_INTERFACE_FINDINGS.md

    def test_shader_headers_and_record(self):
        self.need_src()
        m = re.search(r'shader < (0x[0-9a-f]+)ULL \|\| shader > (0x[0-9a-f]+)ULL', self.src)
        self.assertEqual(self.rt['shader_header_range'], [int(m.group(1), 16) - BASE, int(m.group(2), 16) - BASE])
        m = re.search(r'Read\((0x[0-9a-f]+)ull, &record, 8\)', self.src)
        self.assertEqual(self.rt['shader_record_table'], int(m.group(1), 16) - BASE)
        m = re.search(r'record == (0x[0-9a-f]+)ull', self.src)
        self.assertEqual(self.rt['shader_record_first_header'], int(m.group(1), 16) - BASE)

    def test_description_layout(self):
        self.need_src()
        offsets = re.search(r'kOffsets\[kResCount\] = \{([^}]*)\}', self.src).group(1)
        truth = [int(x, 0) for x in offsets.split(',')]
        A = ffx.ADAPTER_FSR3_V1['description']
        self.assertEqual([A[k] for k in ('color', 'depth', 'motion_vectors', 'exposure', 'reactive',
                                         'transparency_composition', 'output')], truth)
        for f in ('color', 'depth', 'motion_vectors', 'output', 'jitter', 'render_size', 'output_size', 'reset'):
            self.assertIn(f, self.rt['description_fields_seen'])

    def test_profile_selects_ffx_and_keeps_presets(self):
        prof = scan.build_profile(self.g)
        self.assertEqual(prof['selected_route'], 'ffx_static:ffx_agc_fsr3upscaler_v1')
        self.assertEqual(prof['capabilities']['upscaler']['state'], 'available')
        self.assertEqual(prof['capabilities']['frame_generation']['state'], 'available')
        self.assertEqual(prof['capabilities']['render_presets']['state'], 'available')
        drs = prof['runtime']['resolution_controller']
        self.need_src()
        self.assertEqual(drs['table_rva'], int(re.search(r'kDrsTable\s*= (0x[0-9a-f]+)ull', self.src).group(1), 16) - BASE)

    def test_pro_route_is_mfsr2_not_legacy(self):
        abis = {r['abi']: r for r in self.psml['routes']}
        self.assertEqual(set(abis), {'psml_mfsr2'})
        self.assertEqual(abis['psml_mfsr2']['state'], 'static_layout_verified')


@unittest.skipUnless(DEMONS.is_dir() and DEMONS_TRUTH.is_file(), "Demon's Souls or its identifiers not installed")
class DemonsSouls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.truth = json.loads(DEMONS_TRUTH.read_text())
        cls.g, cls.img, cls.psml, cls.ffx = analysed(DEMONS)
        cls.route = cls.psml['routes'][0]
        cls.site = cls.route['callsites'][0]

    def test_hash_matches_truth(self):
        self.assertEqual(self.g.sha256, self.truth['game']['sha256'])

    def test_abi_and_state(self):
        self.assertEqual([r['abi'] for r in self.psml['routes']], ['psml_legacy_r2'])
        self.assertEqual(self.route['entry']['nid'], '94iBp3KvIuI')
        self.assertEqual(self.route['state'], 'static_layout_verified')

    def test_call_chain(self):
        t = self.truth['static_binding_sites']
        self.assertEqual(h(self.site['game_function_rva']), h(t['apply_function_rva']))
        self.assertEqual(h(self.site['game_call_rva']), h(t['apply_dispatch_call_rva']))
        self.assertEqual(h(self.site['chain'][0]['function_rva']), h(t['psml_wrapper_rva']))
        self.assertEqual(h(self.site['chain'][0]['call_rva']), h(t['psml_import_call_rva']))
        self.assertEqual(self.site['parameter_block']['kind'], 'global')
        self.assertEqual(h(self.site['parameter_block']['rva']), h(t['parameter_block_rva']))

    def test_every_truth_store_found(self):
        found = set()
        for e in list(self.site['resources'].values()) + list(self.site['fields'].values()):
            found.update(h(x) for x in e['store_rvas'])
        missing = {k: v['store_instruction_rva'] for k, v in self.truth['static_binding_sites']['stores'].items()
                   if h(v['store_instruction_rva']) not in found}
        self.assertEqual(missing, {})

    def test_no_ffx_route(self):
        self.assertEqual(self.ffx['routes'], [])


@unittest.skipUnless(ASTRO.is_file() and ASTRO_TRUTH.is_file(), 'ASTRO BOT or its identifiers not installed')
class AstroBot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.truth = json.loads(ASTRO_TRUTH.read_text())
        cls.g, cls.img, cls.psml, cls.ffx = analysed(ASTRO)
        cls.route = cls.psml['routes'][0]
        cls.site = cls.route['callsites'][0]

    def test_hash_matches_truth(self):
        self.assertEqual(self.g.sha256, self.truth['game']['sha256'])

    def test_abi_and_hook(self):
        t = self.truth['hook']
        self.assertEqual(self.route['abi'], 'psml_legacy_r1')
        self.assertEqual(self.route['entry']['nid'], t['nid'])
        self.assertEqual(self.route['state'], 'static_layout_verified')
        self.assertEqual(h(self.site['game_function_rva']), h(t['game_render_function_rva']))
        self.assertEqual(h(self.site['game_call_rva']), h(t['game_dispatch_call_rva']))
        self.assertEqual(h(self.site['chain'][0]['function_rva']), h(t['dispatch_wrapper_rva']))
        self.assertEqual(h(self.site['chain'][0]['call_rva']), h(t['import_call_rva']))
        self.assertEqual(self.site['parameter_block']['kind'], 'stack')
        self.assertEqual(self.site['parameter_block']['frame_offset'], -0x180)

    def test_resource_stores(self):
        roles = {'output': 'output', 'color': 'color', 'motion_vectors': 'motion_vectors', 'depth': 'depth',
                 'exposure': 'exposure', 'auxiliary_slot3': 'aux_slot3', 'auxiliary_slot6': 'aux_slot6'}
        for r in self.truth['resources']:
            mine = self.site['resources'][roles.get(r['role'], r['role'])]
            self.assertEqual(h(mine['parameter_offset']), h(r['parameter_offset']), r['role'])
            self.assertIn(h(r['store_instruction_rva']), [h(x) for x in mine['store_rvas']], r['role'])

    def test_temporal_stores(self):
        found = set()
        for e in self.site['fields'].values():
            found.update(h(x) for x in e['store_rvas'])
        for t in self.truth['temporal_store_evidence']:
            self.assertIn(h(t['instruction_rva']), found, t['field'])

    def test_no_ffx_route(self):
        self.assertEqual(self.ffx['routes'], [])


class CrossChecks(unittest.TestCase):
    """A game must never receive another game's ABI."""

    def test_distinct_abis(self):
        seen = {}
        for p in (YOTEI, DEMONS, ASTRO):
            if p.exists():
                seen[p.name] = {r['abi'] for r in analysed(p)[2]['routes']}
        if len(seen) < 2:
            self.skipTest('fewer than two games installed')
        flat = [abi for abis in seen.values() for abi in abis]
        self.assertEqual(len(flat), len(set(flat)), seen)


if __name__ == '__main__':
    unittest.main()
