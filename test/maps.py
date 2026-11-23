#!/usr/bin/env python3
"""Q2 Hero's charts against hand-made osu!mania maps of the same songs.

    test/maps.py SONGS_DIR [LISTEN_TOOL]

SONGS_DIR holds one folder per song, each with a .osu map and the audio it names (an osu! Songs
folder, unpacked). Nothing of them is kept in the repository: they are other people's work.

For each song: the map's tempo against the one found; for each difficulty, the notes a second,
how many sit on a map note (within 40 ms: a "row" is any note or chord of the map), how many of the
map's rows Expert covers, and how many of the chart's bars repeat an earlier bar's rhythm.
"""
import glob, os, re, subprocess, sys
from collections import Counter

DIFFS = ['Easy', 'Medium', 'Hard', 'Expert']


def parse_osu(path):
    sec, keys, audio, red, rows = None, 4, '', [], []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        m = re.match(r'^\[(.+)\]$', line)
        if m:
            sec = m.group(1)
            continue
        if sec == 'General' and line.startswith('AudioFilename:'):
            audio = line.split(':', 1)[1].strip()
        elif sec == 'Difficulty' and line.startswith('CircleSize:'):
            keys = int(float(line.split(':', 1)[1]))
        elif sec == 'TimingPoints':
            p = line.split(',')
            if len(p) >= 2 and (len(p) < 7 or p[6] == '1') and float(p[1]) > 0:
                red.append((float(p[0]), float(p[1])))
        elif sec == 'HitObjects':
            p = line.split(',')
            if len(p) >= 5:
                rows.append(int(p[2]) / 1000)
    return keys, audio, red, sorted(set(rows))


def near(t, v, tol=0.04):
    import bisect
    i = bisect.bisect_left(v, t - tol)
    return i < len(v) and v[i] <= t + tol


def main():
    root = sys.argv[1]
    tool = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '../build/host/listen_tool')
    tot = {d: [0, 0, 0.0] for d in DIFFS}  # on a map row, notes, seconds
    cov = [0, 0]
    for folder in sorted(glob.glob(os.path.join(root, '*/')), key=lambda p: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p)]):
        osu = sorted(glob.glob(os.path.join(folder, '*.osu')))
        if not osu:
            continue
        keys, audio, red, rows = parse_osu(osu[0])
        env = dict(os.environ, HERO_FFMPEG=os.environ.get('HERO_FFMPEG', '/usr/bin/ffmpeg'))
        out = subprocess.run([tool, os.path.join(folder, audio)], capture_output=True, text=True, env=env).stdout
        bpm, dur, notes = 0, 1, {d: [] for d in range(4)}
        for line in out.splitlines():
            p = line.split()
            if p[0] == 'bpm':
                bpm = float(p[1])
            elif p[0] == 'duration':
                dur = float(p[1])
            elif p[0] == 'note':
                notes[int(p[1])].append((float(p[2]), int(p[3])))
        # the map's tempo: the red line holding the most time
        held = Counter()
        ends = [r[0] for r in red[1:]] + [rows[-1] * 1000 if rows else 0]
        for (t, bl), e in zip(red, ends):
            held[round(60000 / bl, 1)] += max(0, e - t)
        mbpm = held.most_common(1)[0][0] if held else 0
        span = (rows[-1] - rows[0]) if rows else dur
        line = f'{os.path.basename(folder.rstrip("/"))[:22]:22} map {mbpm:6.1f} BPM, found {bpm:6.1f} ({bpm / mbpm if mbpm else 0:.2f}x)'
        for d in range(4):
            v = [t for t, _ in notes[d]]
            on = sum(near(t, rows) for t in v)
            tot[DIFFS[d]][0] += on
            tot[DIFFS[d]][1] += len(v)
            tot[DIFFS[d]][2] += span
            line += f' | {DIFFS[d][0]} {len(v) / span:.1f}/s {100 * on / max(1, len(v)):3.0f}%'
        ex = sorted(t for t, _ in notes[3])
        c = sum(near(t, ex) for t in rows)
        cov[0] += c
        cov[1] += len(rows)
        line += f' | map rows covered {100 * c / max(1, len(rows)):3.0f}%  (map {len(rows) / span:.1f}/s)'
        print(line)
    print()
    for d in DIFFS:
        on, n, s = tot[d]
        print(f'{d:7} {n / max(s, 1):.2f} notes/s, {100 * on / max(1, n):.0f}% on a map note')
    print(f'Expert covers {100 * cov[0] / max(1, cov[1]):.0f}% of the maps\' rows')


if __name__ == '__main__':
    main()
