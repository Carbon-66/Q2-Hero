#!/usr/bin/env python3
"""The tune tracker and the charts against mixes with a known tune and strings (test/mktunes.py).
    tunes.py LISTEN_TOOL MIX_DIR
How many of the tune's notes are found right (begun within 60 ms, at the right pitch), and for each
difficulty how much of the tune it charts and how many of the tune's long notes it holds."""
import sys, os, json, subprocess, numpy as np
tool, d = sys.argv[1], sys.argv[2]
env = dict(os.environ, HERO_FFMPEG=os.environ.get('HERO_FFMPEG', '/usr/bin/ffmpeg'))
TP = FP = FN = 0; dur_err = []
cov = np.zeros(4); onv = np.zeros(4); tot = 0; nn = np.zeros(4); hn = np.zeros(4); held = np.zeros(4); longs_total = 0
for k in range(6):
    meta = json.load(open(f'{d}/mix{k}.json')); tr = meta['notes']; bl = 60 / meta['bpm']
    out = subprocess.run([tool, f'{d}/mix{k}.flac'], capture_output=True, text=True, env=env).stdout
    lines = [l.split() for l in out.splitlines()]
    est = [(float(h[1]), float(h[4]), float(h[5])) for h in lines if h and h[0] == 'hit' and h[3] == '0']
    used = set(); tp = 0
    for t, du, m in tr:
        c = [j for j, e in enumerate(est) if abs(e[0] - t) < 0.06 and abs(e[1] - m) < 0.6 and j not in used]
        if c:
            j = min(c, key=lambda j: abs(est[j][0] - t)); used.add(j); tp += 1; dur_err.append(est[j][2] / du)
    TP += tp; FP += len(est) - tp; FN += len(tr) - tp
    longs = [n for n in tr if n[1] >= 0.75 * bl]; longs_total += len(longs); tot += len(tr)
    for df in range(4):
        v = [(float(x[2]), float(x[4])) for x in lines if x and x[0] == 'note' and int(x[1]) == df]
        ts = np.array([a for a, _ in v])
        cov[df] += sum(np.min(np.abs(ts - n[0])) < 0.05 for n in tr) if len(ts) else 0
        onv[df] += sum(min(abs(x - n[0]) for n in tr) < 0.05 for x in ts)
        nn[df] += len(v); hn[df] += sum(1 for _, L in v if L > 0)
        held[df] += sum(any(abs(a - n[0]) < 0.05 and L > 0 for a, L in v) for n in longs)
P, R = TP / max(1, TP + FP), TP / max(1, TP + FN)
print(f'tune notes found right: {R:.2f} of the tune; {P:.2f} of those found; found length / true {np.median(dur_err):.2f}')
for df, name in enumerate(['Easy', 'Medium', 'Hard', 'Expert']):
    print(f'{name:7} charts {cov[df] / tot:.2f} of the tune ({onv[df] / nn[df]:.2f} of its notes on it); holds {hn[df] / nn[df]:.2f} of its notes and {held[df] / longs_total:.2f} of the tune\'s long notes')

# held notes: how many land on something held (a long sung note, a string note, the pad's change),
# and how many of the long sung notes and the string notes are held
H = np.zeros(4); SH = np.zeros(4); SL = [0]; ok = np.zeros(4); longs_held = np.zeros(4); L = 0; nn = np.zeros(4)
for k in range(6):
    m = json.load(open(f'{d}/mix{k}.json')); tr = m['notes']; bl = 60 / m['bpm']
    longs = [n for n in tr if n[1] >= 0.75 * bl]; L += len(longs)
    st = m.get('strings', []); SL[0] += len(st)
    bars = [1.0 + b * 4 * bl for b in range(16)]  # the pad changes each bar
    out = subprocess.run([tool, f'{d}/mix{k}.flac'], capture_output=True, text=True, env=env).stdout
    for x in (l.split() for l in out.splitlines()):
        if not x or x[0] != 'note': continue
        df, t, ln = int(x[1]), float(x[2]), float(x[4]); nn[df] += 1
        if ln <= 0: continue
        H[df] += 1
        good = any(abs(t - n[0]) < 0.05 and n[1] >= 0.6 * ln for n in tr + st) or any(abs(t - b) < 0.05 for b in bars)
        SH[df] += any(abs(t - n[0]) < 0.05 for n in st)
        ok[df] += good
        longs_held[df] += any(abs(t - n[0]) < 0.05 for n in longs)
for df, nm in enumerate(['Easy', 'Medium', 'Hard', 'Expert']):
    print(f'  {nm:7} held {H[df] / nn[df]:.2f} of notes; on a sustained sound {ok[df] / max(1, H[df]):.2f}; long sung notes held {longs_held[df] / L:.2f}; string notes held {SH[df] / max(1, SL[0]):.2f}')
