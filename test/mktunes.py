"""Realistic-ish mixes with a known melody, for the tune tracker: a sung-like lead (formants, vibrato,
glides, breathy start) in the centre with a little stereo reverb, against plucked arpeggios and
strummed chords in the same range panned left and right, a wide string pad, bass and drums, and in
the second half a bowed string section holding long notes, spread wide.
Used to tune the tune tracker (src/analyze.c) and to score it: test/tunes.py.
mktunes.py OUTDIR  -> mixN.flac and mixN.json (notes: t, dur, midi)"""
import json, os, subprocess, sys
import numpy as np
from scipy.signal import lfilter

SR = 44100
out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def mtof(m): return 440 * 2 ** ((m - 69) / 12)


def resonator(x, f, bw):
    r = np.exp(-np.pi * bw / SR); th = 2 * np.pi * f / SR
    return lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], x)


def sing(pitch_curve, amp, vowel):
    """a saw following pitch_curve (Hz per sample), through three formants"""
    ph = np.cumsum(pitch_curve / SR)
    x = 2 * (ph % 1) - 1
    F = {'a': (750, 1200, 2600), 'e': (450, 1900, 2600), 'o': (500, 900, 2500), 'i': (300, 2200, 3000)}[vowel]
    y = sum(g * resonator(x, f, 90 + f * 0.05) for f, g in zip(F, (1.0, 0.6, 0.3)))
    return y * amp


def pluck(f, n, bright=0.6, decay=1.2):
    t = np.arange(n) / SR
    y = sum((bright ** (h - 1)) / h * np.sin(2 * np.pi * f * h * t) * np.exp(-t * decay * (1 + 0.6 * h)) for h in range(1, 9))
    return y * np.minimum(t / 0.003, 1)


def song(k, bpm, key, bars, acc_gain, rng):
    beat = 60 / bpm
    start = 1.0
    total = start + bars * 4 * beat + 3
    n = int(total * SR)
    L = np.zeros(n); R = np.zeros(n)
    def add(t, sig, gl, gr):
        i = int(t * SR); m = min(len(sig), n - i)
        if m > 0: L[i:i + m] += sig[:m] * gl; R[i:i + m] += sig[:m] * gr
    scale = [0, 2, 4, 5, 7, 9, 11]
    def deg(d): return key + 12 * (d // 7) + scale[d % 7]
    prog = [0, 5, 3, 4]  # I vi IV V
    # the melody: phrases of 2 bars, rhythms from a set, a random walk in the scale (mostly steps)
    notes = []
    d = 3 + rng.integers(0, 3)
    rhythms = [[1, 1, 1, 1, 2, 2], [0.5, 0.5, 1, 1, 0.5, 0.5, 1, 3], [1.5, 0.5, 1, 1, 4], [0.5] * 6 + [1, 2, 2],
               [2, 1, 1, 2, 2], [1, 0.5, 0.5, 2, 1, 3]]
    b = 0.0
    while b < bars * 4 - 8:
        r = rhythms[rng.integers(0, len(rhythms))]
        for i, dur in enumerate(r):
            if rng.random() < 0.08 and i:  # a rest
                b += dur; continue
            step = rng.choice([-2, -1, -1, 0, 1, 1, 2, 3, -3, 4], p=[.1, .2, .1, .1, .2, .1, .08, .04, .04, .04])
            d = int(np.clip(d + step, 0, 10))
            notes.append((b, dur, deg(d)))
            b += dur
        b += rng.choice([0, 0.5, 1])  # a breath between phrases
    # the voice: one continuous pitch curve per note, glide 40 ms from the last, vibrato late in long notes
    for i, (b, dur, m) in enumerate(notes):
        t0 = start + b * beat
        nl = int(dur * beat * 0.95 * SR)
        tt = np.arange(nl) / SR
        f = np.full(nl, mtof(m))
        prev = notes[i - 1] if i and abs(notes[i - 1][0] + notes[i - 1][1] - b) < 0.01 else None
        if prev: f *= 2 ** ((prev[2] - m) / 12 * np.exp(-tt / 0.03))
        f *= 1 + 0.012 * np.sin(2 * np.pi * 5.6 * tt) * np.clip((tt - 0.25) / 0.3, 0, 1)
        f *= 1 + 0.002 * rng.standard_normal(nl).cumsum() / np.sqrt(np.arange(1, nl + 1))  # wander
        amp = np.minimum(tt / 0.04, 1) * np.minimum((nl - np.arange(nl)) / (0.05 * SR), 1) * (0.8 + 0.2 * rng.random())
        v = sing(f, amp, 'aeoi'[rng.integers(0, 4)])
        breath = rng.standard_normal(nl) * np.exp(-tt / 0.03) * 0.15
        add(t0, v + resonator(breath, 3000, 2000), 0.5, 0.5)
        for dl, gl, gr in ((0.031, 0.18, 0.05), (0.047, 0.05, 0.18), (0.083, 0.1, 0.03), (0.11, 0.03, 0.1)):  # reverb
            add(t0 + dl, v, gl * 0.6, gr * 0.6)
    # strings: a bowed section (detuned saws, slow attack) holding long notes, 1-4 beats, under the
    # second half of the song, spread wide; their notes are kept as truth too
    strings = []
    sb = bars // 2 * 4
    while sb < bars * 4 - 4:
        dur = float(rng.choice([1, 2, 2, 3, 4]))
        m = int(deg(prog[int(sb // 4) % 4] + int(rng.choice([0, 2, 4])))) + 12
        strings.append((sb, dur, m))
        nl = int(dur * beat * SR); tt = np.arange(nl) / SR
        env = np.minimum(tt / 0.08, 1) * np.minimum((nl - np.arange(nl)) / (0.1 * SR), 1)
        f = mtof(m) * (1 + 0.004 * np.sin(2 * np.pi * 5 * tt))
        for dt, gl, gr in ((0.996, 0.8, 0.2), (1.004, 0.2, 0.8), (1.0, 0.5, 0.5)):
            ph = np.cumsum(f * dt / SR)
            saw = resonator(2 * (ph % 1) - 1, 1200, 1500)
            add(start + sb * beat, saw * env * 0.25 * acc_gain, gl, gr)
        sb += dur
    # accompaniment: arpeggio left (same octave as the tune), strums right, a wide pad
    for bar in range(bars):
        root = deg(prog[bar % 4])
        chord = [root, root + (3 if prog[bar % 4] in (5,) else 4), root + 7]
        for e in range(8):
            t = start + (bar * 4 + e * 0.5) * beat
            m = chord[[0, 1, 2, 1][e % 4]] + 12 + (12 if e % 4 == 2 else 0)
            add(t, pluck(mtof(m), int(beat * 1.5 * SR)), 0.75 * acc_gain, 0.15 * acc_gain)
        for s in (0, 1.5, 2, 3):
            t = start + (bar * 4 + s) * beat
            for j, m in enumerate(sorted(chord) + [chord[0] + 12]):
                add(t + j * 0.012, pluck(mtof(m + 12), int(beat * 1.2 * SR), 0.8, 2.0) * 0.5, 0.15 * acc_gain, 0.7 * acc_gain)
        nl = int(4 * beat * SR); tt = np.arange(nl) / SR
        pad = sum(np.sin(2 * np.pi * mtof(m + 12) * tt * dt) for m in chord for dt in (0.997, 1.003))
        pe = np.minimum(tt / 0.3, 1) * np.minimum((nl - np.arange(nl)) / (0.2 * SR), 1)
        add(start + bar * 4 * beat, pad * pe * 0.04 * acc_gain, 0.9, 0.1)
        add(start + bar * 4 * beat + 0.013, pad * pe * 0.04 * acc_gain, 0.1, 0.9)
        # bass: roots on beats 1 and 3, the fifth on the "and" of 4
        for s, mm in ((0, root - 24), (2, root - 24), (3.5, root - 17)):
            nl = int(beat * 1.4 * SR); tt = np.arange(nl) / SR
            bs = (np.sin(2 * np.pi * mtof(mm) * tt) + 0.3 * np.sin(4 * np.pi * mtof(mm) * tt)) * np.exp(-tt * 2)
            add(start + (bar * 4 + s) * beat, bs * 0.5, 0.5, 0.5)
        for s in range(4):
            t = start + (bar * 4 + s) * beat
            nl = int(0.25 * SR); tt = np.arange(nl) / SR
            if s % 2 == 0: add(t, np.sin(2 * np.pi * (50 + 80 * np.exp(-tt * 30)) * tt) * np.exp(-tt * 9) * 0.7, 0.5, 0.5)
            else: add(t, rng.standard_normal(nl) * np.exp(-tt * 15) * 0.25, 0.5, 0.5)
            for h in (0, 0.5):
                nh = int(0.05 * SR)
                add(t + h * beat, np.diff(rng.standard_normal(nh), prepend=0) * np.exp(-np.arange(nh) / SR * 60) * 0.12, 0.6, 0.4)
    y = np.stack([L, R], 1)
    y = y / np.max(np.abs(y)) * 0.85
    wav = os.path.join(out, f'mix{k}.wav')
    import wave
    with wave.open(wav, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((y * 32767).astype('<i2').tobytes())
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, os.path.join(out, f'mix{k}.flac')], check=True)
    os.unlink(wav)
    json.dump({'bpm': bpm, 'notes': [(start + b * beat, d * beat, m) for b, d, m in notes],
               'strings': [(start + b * beat, d * beat, m) for b, d, m in strings]}, open(os.path.join(out, f'mix{k}.json'), 'w'))


rng = np.random.default_rng(3)
for k, (bpm, key, gain) in enumerate([(92, 60, 0.6), (120, 62, 0.8), (140, 57, 1.0), (165, 64, 0.8), (105, 59, 1.2), (128, 55, 1.0)]):
    song(k, bpm, key, 16, gain, rng)
    print('mix', k, bpm)
