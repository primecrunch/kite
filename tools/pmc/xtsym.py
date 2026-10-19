# xtsym.py TIME_SAMPLE_XML COUNTERS_XML SYMS PROCNAME: leaf-function sample counts on P and E cores
# (call stacks from the time-sample table, cores from the counters table, joined on time and thread).
import re, sys, collections, bisect
xml, cxml, symf, pname, slidef = sys.argv[1:6]
syms = []
for l in open(symf):
    a, t, n = l.split()[:3]
    syms.append((int(a, 16), n))
addrs = [a for a, _ in syms]
text0, text1 = 0x100000000, 0x100000000 + 0x38000
ids = {}  # element id -> fmt text or value
rows = []
row_re = re.compile(r'<row>(.*?)</row>', re.S)
el_re = re.compile(r'<(sample-time|tid|core|thread|text-address|kperf-bt)(?=[ >/])( id="(\d+)")?( fmt="([^"]*)")?( ref="(\d+)")?[^>]*?(/?)>([^<]*)')
def parse(path):
    ids = {}
    btpc = {}  # kperf-bt id -> leaf pc (a repeated stack is a ref to its first copy)
    out = []
    for m in row_re.finditer(open(path).read()):
        r = m.group(1)
        f = {}
        bt = None
        for e in el_re.finditer(r):
            tag, eid, fmt, ref, body = e.group(1), e.group(3), e.group(5), e.group(7), e.group(9)
            v = body if tag in ('text-address', 'sample-time', 'tid') else fmt
            if eid: ids[eid] = v
            if ref: v = ids.get(ref)
            if tag == 'kperf-bt':
                if eid: bt = eid
                elif ref and ref in btpc and 'text-address' not in f: f['text-address'] = btpc[ref]
            if tag == 'text-address' and bt and bt not in btpc: btpc[bt] = v
            if tag not in f: f[tag] = v
        out.append(f)
    return out
cores = collections.defaultdict(list)  # tid -> [(time, core)]
for f in parse(cxml):
    if f.get('core') and f.get('thread') and pname in f['thread']:
        cores[f.get('tid')].append((int(f['sample-time']), 'E' if 'E Core' in f['core'] else 'P'))
for v in cores.values(): v.sort()
def core_at(tid, t):
    v = cores.get(tid)
    if not v: return None
    i = bisect.bisect_left(v, (t, ''))
    best = min((abs(v[j][0] - t), v[j][1]) for j in (i - 1, i) if 0 <= j < len(v))
    return best[1] if best[0] <= 500000 else None  # within 0.5 ms
rows = []
nts = 0
for f in parse(xml):
    if f.get('thread') and pname in f['thread']: nts += 1
    if f.get('thread') and pname in f['thread'] and f.get('text-address'):
        c = core_at(f.get('tid'), int(f['sample-time']))
        if c:
            pid = re.search(r'pid: (\d+)', f['thread']).group(1)
            rows.append((pid, c, int(f['text-address'])))
print("time samples", nts, "joined", len(rows), "counter samples", sum(len(v) for v in cores.values()))
by = collections.defaultdict(list)
for pid, c, pc in rows: by[pid].append(pc)
slide = {}
for l in open(slidef):
    pid, rng = l.split()
    slide[pid] = int(rng.split('-')[0], 16) - 0x100000000
cnt = collections.defaultdict(lambda: [0, 0])
for pid, c, pc in rows:
    if pid not in slide: continue
    a = pc - slide[pid]
    if not (text0 <= a < text1): name = '(system libraries)'
    else:
        i = bisect.bisect_right(addrs, a) - 1
        name = syms[i][1] if i >= 0 else '?'
    cnt[name][0 if c == 'P' else 1] += 1
tp = sum(v[0] for v in cnt.values()); te = sum(v[1] for v in cnt.values())
print(f"samples P {tp} E {te}; slides found for {len(slide)}/{len(by)} pids")
print(f"{'function':40s} {'P%':>6} {'E%':>6} {'E/P share':>9}")
for n, (p, e) in sorted(cnt.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:25]:
    print(f"{n[:40]:40s} {100*p/tp:6.2f} {100*e/te:6.2f} {(e/te)/(p/tp) if p else 0:9.2f}")
