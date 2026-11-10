from pathlib import Path
import time,os,json
base=Path(__file__).parent
end=time.monotonic()+360
out=base/'ram-samples.jsonl'
while time.monotonic()<end and not (base/'ram-stop').exists():
 groups={k:[] for k in ('codex','launcher','build','game','helix')}
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   exe=os.readlink(p/'exe');name=(p/'comm').read_text().strip();group=None
   if '/.codex/packages/' in exe:group='codex'
   elif name=='KytyPS5':group='launcher'
   elif '/kyty_emulator_' in exe:group='game'
   elif name in ('clang++','clang','ld.lld','lld'):group='build'
   elif 'hlx_server' in name:group='helix'
   if group is None:continue
   vals={}
   for l in (p/'smaps_rollup').read_text().splitlines():
    if ':' not in l:continue
    k,v=l.split(':',1)
    if k in ('Rss','Pss','Swap','SwapPss'):vals[k]=int(v.split()[0])/1024
   vals.update(pid=int(p.name),name=name);groups[group].append(vals)
  except (OSError,ValueError):continue
 try: phase=(base/'ram-phase').read_text().strip()
 except OSError:phase='working'
 with out.open('a') as f:f.write(json.dumps({'time':time.time(),'phase':phase,'groups':groups})+'\n')
 time.sleep(1)
