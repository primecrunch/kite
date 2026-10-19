# xtcols.py COUNTERS_XML PROCNAME: deltas of each raw counter column between consecutive samples
# of the same thread on the same core, to identify cycles and instructions.
import re, sys, collections, statistics
xml, pname = sys.argv[1:3]
row_re = re.compile(r'<row>(.*?)</row>', re.S)
el_re = re.compile(r'<(sample-time|tid|core|thread|pmc-events)(?=[ >/])( id="(\d+)")?( fmt="([^"]*)")?( ref="(\d+)")?[^>]*?(/?)>([^<]*)')
ids = {}
last = {}
d = collections.defaultdict(list)
for m in row_re.finditer(open(xml).read()):
    f = {}
    for e in el_re.finditer(m.group(1)):
        tag, eid, fmt, ref, body = e.group(1), e.group(3), e.group(5), e.group(7), e.group(9)
        v = body if tag in ('sample-time', 'tid', 'pmc-events') else fmt
        if eid: ids[eid] = v
        if ref: v = ids.get(ref)
        if tag not in f: f[tag] = v
    if not (f.get('thread') and pname in f['thread'] and f.get('pmc-events') and f.get('core')): continue
    t = int(f['sample-time']); vals = [int(x) for x in f['pmc-events'].split()]
    key = f['thread']
    if key in last and last[key][1] == f['core']:
        lt, lc, lv = last[key]
        dt = t - lt
        if 500000 < dt < 1500000:
            d['E' if 'E Core' in f['core'] else 'P'].append([b - a for a, b in zip(lv, vals)] + [dt])
    last[key] = (t, f['core'], vals)
for k, rows in d.items():
    print(k, len(rows), "median deltas per sample:", [int(statistics.median(r[i] for r in rows)) for i in range(len(rows[0]))])
