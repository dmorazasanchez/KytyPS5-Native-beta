import json, sys, os, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from kyty_upscaler_scan import game, selfelf, x86, ffx
def conv(o):
    if isinstance(o, dict): return {k: conv(v) for k, v in o.items()}
    if isinstance(o, (list, tuple, set)): return [conv(v) for v in o]
    if isinstance(o, int) and not isinstance(o, bool) and (o > 0xff or o < -0xff): return hex(o)
    return o
for p in sys.argv[1:]:
    t = time.time()
    g = game.load(p); img = selfelf.SelfImage(g.eboot); d = x86.Disassembler(img)
    r = ffx.analyse(img, d)
    print(g.title_id, '%.1fs' % (time.time() - t), json.dumps(conv(r), indent=1))
