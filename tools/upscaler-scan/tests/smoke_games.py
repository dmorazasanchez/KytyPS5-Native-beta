"""Manual smoke check: parse the installed games (not part of the unit tests)."""
import sys, time
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.dirname(__file__)))
from kyty_upscaler_scan import game, selfelf
GAMES = ['/home/david/Downloads/Demons Souls (v01.007) (Deluxe Edition) – BP 4.xx – PPSA01341 – FPKG/EP9000-PPSA01341_00-DEMONSSOULS00000-files',
         '/home/david/Downloads/Ghost of Yōtei Complete Edition/UP9000-PPSA26344_00-GHOST2SHIP000000-files',
         '/home/david/Downloads/[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018/[PS5] ASTRO BOT (2024) (PPSA21567) (EU) v1.018.zar']
for p in GAMES:
    t = time.time(); g = game.load(p); t1 = time.time()
    img = selfelf.SelfImage(g.eboot); t2 = time.time()
    print(g.title_id, g.version, g.sha256[:12], len(g.eboot) >> 20, 'MiB', 'load %.1fs parse %.1fs' % (t1 - t, t2 - t1))
    print('  libs', sorted(n for n, v in img.libraries.values()))
    print('  imports', len(img.imports), 'relative', len(img.relative), 'plt', len(img.plt))
    for nid in ['RUNLFro+qok', '94iBp3KvIuI', 'lrJwpLjKXRc', 'k3GhuSNmBLU']:
        i = img.imports.get(nid)
        if i: print('  ', nid, i.library, i.module, hex(i.got_rva), [hex(x) for x in i.plt_rvas])
    print('  attribute3', g.param.get('attribute3'), 'attribute2', g.param.get('attribute2'))
