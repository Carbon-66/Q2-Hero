#!/usr/bin/env python3
"""listen_tool's output for a test song against the song's truth: the tempo, how far the beats and
hits sit from where they are, how many of the true hits each difficulty's notes land on, and the
charts' shape (on the grid, the lanes, repeating bars).
Usage: accuracy.py TOOL_OUTPUT TRUTH  (prints 'key value' lines; exits 1 on a failure)"""
import json, statistics, sys
out, truth = open(sys.argv[1]).read().split('\n'), json.load(open(sys.argv[2]))
beats = [float(l.split()[1]) for l in out if l.startswith('beat ')]
hits = [float(l.split()[1]) for l in out if l.startswith('hit ')]
notes = {}
for l in out:
    if l.startswith('note '):
        _, d, t, lanes, ln, hero = l.split()
        notes.setdefault(int(d), []).append((float(t), int(lanes), float(ln)))
bpm = float([l for l in out if l.startswith('bpm ')][0].split()[1])
true_hits = sorted(set(round(t, 4) for t, _ in truth['hits']))  # notes may land on any of these
main_hits = sorted(set(round(t, 4) for t, k in truth['hits'] if k != 'hat'))  # and these must be heard
true_beats = [truth['start'] + i * truth['beat'] for i in range(int((max(main_hits) - truth['start']) / truth['beat']) + 1)]
def offsets(found, ref, tol):
    d = []
    for r in ref:
        c = min(found, key=lambda f: abs(f - r)) if found else None
        if c is not None and abs(c - r) <= tol: d.append(c - r)
    return d
fail = 0
print('bpm', round(bpm, 2), 'true', truth['bpm'])
ok_bpm = min(abs(bpm - truth['bpm'] * k) for k in (1, 2, 0.5)) / truth['bpm'] < 0.02
fail |= not ok_bpm
bo = offsets(beats, true_beats, 0.08)
print('beats_found', f'{len(bo)}/{len(true_beats)}', 'mean_ms', round(1000 * statistics.mean(bo), 1) if bo else '-',
      'sd_ms', round(1000 * statistics.pstdev(bo), 1) if len(bo) > 1 else '-')
ho = offsets(hits, main_hits, 0.05)
print('hits_found', f'{len(ho)}/{len(main_hits)}', 'mean_ms', round(1000 * statistics.mean(ho), 1), 'sd_ms', round(1000 * statistics.pstdev(ho), 1))
fail |= len(bo) < 0.85 * len(true_beats) or len(ho) < 0.8 * len(main_hits)
for d in sorted(notes):
    ts = [t for t, _, _ in notes[d]]
    on = offsets(true_hits, ts, 0.04)  # notes that sit on a real hit
    lanes = [l for _, l, _ in notes[d]]
    print(f'diff{d}', 'notes', len(ts), 'on_hits', f'{len(on)}/{len(ts)}',
          'mean_ms', round(1000 * statistics.mean(on), 1) if on else '-', 'lanes', {k: lanes.count(k) for k in sorted(set(lanes))},
          'held', sum(1 for _, _, ln in notes[d] if ln > 0))
    fail |= len(on) < (0.9 if d <= 1 else 0.85) * len(ts)  # Hard and Expert take in the less certain parts too
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    print(f'diff{d}_min_gap_ms', round(1000 * min(gaps)) if gaps else '-')
# the charts' shape, as in hand-made maps (test/maps.py): notes on the grid (quarters or thirds), Easy on
# the beat, every lane in use and none hogged, Prev + Next the only chord, and bars that repeat
step = truth['beat']
lag = statistics.mean(bo) if bo else 0  # the found grid's own offset (attack starts, not peaks)
def place(t):  # the place in the beat in twelfths, on quarters (0 3 6 9) or thirds (0 4 8); None off the grid
    x = (t - lag - truth['start']) / step
    q = round(x * 12)
    return q % 12 if abs(x * 12 - q) * step / 12 < 0.04 and q % 12 in (0, 3, 4, 6, 8, 9) else None
for d in sorted(notes):
    ns = notes[d]
    pl = [place(t) for t, _, _ in ns]
    on_grid = sum(p is not None for p in pl) / len(ns)
    on_beat = sum(p == 0 or ln > 0 for p, (_, _, ln) in zip(pl, ns)) / len(ns)  # Easy sings along off the beat only to hold
    lanes = [l for _, l, _ in ns]
    singles = [l for l in lanes if l in (1, 2, 4)]
    share = max(singles.count(k) for k in (1, 2, 4)) / max(1, len(singles))
    bars = {}
    for (t, l, _), p in zip(ns, pl):
        if p is None: continue
        q = round((t - lag - truth['start']) / step * 12)
        bars.setdefault(q // 48, set()).add(q % 48)
    keys = sorted(bars)
    alike = lambda x, y: len(x & y) >= 0.8 * len(x | y)  # nearly the same rhythm: a note or so apart
    rep = sum(1 for i, b in enumerate(keys) if any(alike(bars[b], bars[c]) for c in keys[:i])) / max(1, len(keys))
    print(f'diff{d}_shape', 'on_grid', round(on_grid, 2), 'on_beat', round(on_beat, 2), 'top_lane', round(share, 2),
          'bars_repeating', round(rep, 2))
    if on_grid < 0.9: print(f'FAIL diff{d}: {on_grid:.2f} of the notes on the grid'); fail = 1
    if d == 0 and on_beat < 0.9: print(f'FAIL diff{d}: Easy has {on_beat:.2f} of its notes on the beat or held'); fail = 1
    if any(l not in (1, 2, 4, 5) for l in lanes): print(f'FAIL diff{d}: a chord Q2 Pod would drop'); fail = 1
    if share > 0.6 or len(set(singles)) < 3: print(f'FAIL diff{d}: lanes {dict((k, singles.count(k)) for k in (1, 2, 4))}'); fail = 1
    if len(keys) >= 12 and rep < 0.4: print(f'FAIL diff{d}: only {rep:.2f} of the bars repeat an earlier rhythm'); fail = 1
sys.exit(1 if fail else 0)
