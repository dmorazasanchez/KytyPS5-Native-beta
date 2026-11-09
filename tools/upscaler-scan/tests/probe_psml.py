import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from kyty_upscaler_scan import game, selfelf, x86, psml
for p in sys.argv[1:]:
    g = game.load(p); img = selfelf.SelfImage(g.eboot); d = x86.Disassembler(img)
    r = psml.analyse(img, d)
    def conv(o):
        if isinstance(o, dict): return {k: conv(v) for k, v in o.items()}
        if isinstance(o, (list, tuple)): return [conv(v) for v in o]
        if isinstance(o, int) and not isinstance(o, bool) and o > 0xffff: return hex(o)
        return o
    print(g.title_id, json.dumps(conv(r), indent=1)[:6000])
