#!/usr/bin/env python3
"""Test songs with known beats and notes: a band (kick, snare, hats, bass, a lead with held
notes) at a known tempo, written as WAV, then MP3 and FLAC by ffmpeg, with its hits in .truth
(time and part). Usage: mksongs.py OUTDIR"""
import json, os, subprocess, sys
import numpy as np

SR = 44100
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(7)

def env(n, a, d):
    t = np.arange(n) / SR
    return np.minimum(t / a, 1) * np.exp(-t / d)

def song(name, bpm, bars, start=1.0, swing=0.0, title=None, artist=None):
    beat = 60 / bpm
    total = start + bars * 4 * beat + 2.5
    y = np.zeros(int(total * SR) + SR)
    truth = []
    def add(t, sig, gain=1.0):
        i = int(t * SR)
        y[i:i + len(sig)] += sig[:len(y) - i] * gain
    n = int(0.3 * SR); tt = np.arange(n) / SR
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-tt * 30)) * tt) * env(n, 0.001, 0.12)
    snare = (rng.standard_normal(n) * env(n, 0.001, 0.07) * 0.6 + np.sin(2 * np.pi * 190 * tt) * env(n, 0.001, 0.05))
    hat = np.diff(rng.standard_normal(n // 4), prepend=0) * env(n // 4, 0.0005, 0.02) * 0.35
    scale = [0, 2, 3, 5, 7, 8, 10, 12]
    melody = [0, 3, 5, 7, 5, 3, 7, 10, 12, 10, 7, 5, 3, 5, 7, 3]
    for bar in range(bars):
        for b in range(4):
            t = start + (bar * 4 + b) * beat
            if b in (0, 2): add(t, kick, 0.9); truth.append((t, 'kick'))
            if b in (1, 3): add(t, snare, 0.6); truth.append((t, 'snare'))
            for h in range(2):
                th = t + h * beat / 2 + (swing * beat / 2 if h else 0)
                add(th, hat)
                truth.append((th, 'hat'))
            # bass on each beat
            f = 55 * 2 ** (([0, 0, 5, 7][bar % 4]) / 12)
            m = int(beat * 0.9 * SR); bt = np.arange(m) / SR
            add(t, np.sign(np.sin(2 * np.pi * f * bt)) * env(m, 0.005, 0.25) * 0.18)
        # a lead: eighths for two bars, held notes for two
        if bar % 4 < 2:
            for e in range(8):
                t = start + bar * 4 * beat + e * beat / 2
                note = melody[(bar * 8 + e) % 16]
                f = 440 * 2 ** (note / 12)
                m = int(beat / 2 * 0.95 * SR); lt = np.arange(m) / SR
                add(t, (np.sin(2 * np.pi * f * lt) + 0.4 * np.sin(4 * np.pi * f * lt)) * env(m, 0.004, 0.12) * 0.22)
                truth.append((t, 'lead'))
        elif bar >= 2:
            for e in range(2):
                t = start + bar * 4 * beat + e * 2 * beat
                note = [12, 7][e]
                f = 440 * 2 ** (note / 12)
                m = int(beat * 1.9 * SR); lt = np.arange(m) / SR
                add(t, (np.sin(2 * np.pi * f * lt) + 0.3 * np.sin(4 * np.pi * f * lt)) * env(m, 0.01, 2.0) * 0.25)
                truth.append((t, 'held'))
    y = y / np.max(np.abs(y)) * 0.8
    pcm = (np.stack([y, y], 1) * 32767).astype('<i2')
    wav = os.path.join(out, name + '.wav')
    import wave
    with wave.open(wav, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
    meta = []
    if title: meta += ['-metadata', f'title={title}']
    if artist: meta += ['-metadata', f'artist={artist}']
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, *meta, '-b:a', '192k', os.path.join(out, name + '.mp3')], check=True)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, *meta, os.path.join(out, name + '.flac')], check=True)
    os.unlink(wav)
    json.dump({'bpm': bpm, 'start': start, 'beat': beat, 'hits': sorted(truth)}, open(os.path.join(out, name + '.truth'), 'w'))

def mtof(m):
    return 440 * 2 ** ((m - 69) / 12)

def voice(f0, n, kind, vib=0.0):
    """a lead (a bright saw, filtered, with vibrato after 150 ms), a bass (a darker saw) or a pad"""
    t = np.arange(n) / SR
    if kind == 'lead':
        vibr = 1 + (2 ** (vib / 12) - 1) * np.sin(2 * np.pi * 5.5 * t) * np.clip((t - 0.15) / 0.2, 0, 1)
        ph = np.cumsum(f0 * vibr / SR)
        x = 2 * (ph % 1) - 1
        a = 0.25
    else:
        ph = np.cumsum(np.full(n, f0) / SR)
        x = 2 * (ph % 1) - 1
        a = 0.06 if kind == 'bass' else 0.02
    y = np.zeros(n); acc = 0.0
    for i in range(n):  # one-pole low-pass
        acc += (x[i] - acc) * a
        y[i] = acc
    return y

def score_song(name, bpm, lead, bass, chords, bars, start=0.8, hats16=False, title=None, artist=None, quiet_drums=False):
    """a song from a score: lead and bass as (beat, midi, beats), chords as (beat, [midis], beats)"""
    beat = 60 / bpm
    total = start + bars * 4 * beat + 2.5
    y = np.zeros(int(total * SR) + SR)
    truth, notes = [], []
    def add(t, sig, gain=1.0):
        i = int(t * SR)
        y[i:i + len(sig)] += sig[:len(y) - i] * gain
    n = int(0.3 * SR); tt = np.arange(n) / SR
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-tt * 30)) * tt) * env(n, 0.001, 0.12)
    snare = (rng.standard_normal(n) * env(n, 0.001, 0.07) * 0.6 + np.sin(2 * np.pi * 190 * tt) * env(n, 0.001, 0.05))
    hat = np.diff(rng.standard_normal(n // 4), prepend=0) * env(n // 4, 0.0005, 0.02) * 0.3
    dg = 0.45 if quiet_drums else 0.8
    for b in range(bars * 4):
        t = start + b * beat
        if b % 2 == 0: add(t, kick, dg); truth.append((t, 'kick'))
        else: add(t, snare, dg * 0.7); truth.append((t, 'snare'))
        for h in range(4 if hats16 else 2):
            th = t + h * beat / (4 if hats16 else 2)
            add(th, hat, dg); truth.append((th, 'hat'))
    for part, line in (('lead', lead), ('bass', bass)):
        for (b, m, d) in line:
            t = start + b * beat
            nlen = int(d * beat * 0.92 * SR)
            sig = voice(mtof(m), nlen, part, vib=0.25 if part == 'lead' else 0)
            e = np.minimum(np.arange(nlen) / (0.012 * SR), 1) * np.minimum((nlen - np.arange(nlen)) / (0.03 * SR), 1)
            e *= 1.0 if part == 'lead' else np.maximum(np.exp(-np.arange(nlen) / SR / 0.35), 0.55)
            add(t, sig * e, 0.55 if part == 'lead' else 0.6)
            truth.append((t, part)); notes.append((t, part, m, d * beat))
    for (b, ms, d) in chords:
        t = start + b * beat
        nlen = int(d * beat * SR)
        e = np.minimum(np.arange(nlen) / (0.25 * SR), 1) * np.minimum((nlen - np.arange(nlen)) / (0.1 * SR), 1)
        for m in ms:
            add(t, voice(mtof(m), nlen, 'pad') * e, 0.12)
    y = y / np.max(np.abs(y)) * 0.8
    pcm = (np.stack([y, y], 1) * 32767).astype('<i2')
    wav = os.path.join(out, name + '.wav')
    import wave
    with wave.open(wav, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
    meta = (['-metadata', f'title={title}'] if title else []) + (['-metadata', f'artist={artist}'] if artist else [])
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, *meta, '-b:a', '192k', os.path.join(out, name + '.mp3')], check=True)
    os.unlink(wav)
    json.dump({'bpm': bpm, 'start': start, 'beat': beat, 'hits': sorted(truth), 'notes': sorted(notes)},
              open(os.path.join(out, name + '.truth'), 'w'))

# Melody 110: a tune with repeated notes, steps, leaps and held notes; the bass walks under it,
# and takes a riff of its own in bars 5-6 and 13-14, where the tune rests; a chord pad throughout
A = [(0, 64, .5), (.5, 64, .5), (1, 67, .5), (1.5, 69, .5), (2, 71, 1), (3, 69, 1),
     (4, 67, .5), (4.5, 67, .5), (5, 64, .5), (5.5, 62, .5), (6, 64, 2)]
B = [(8, 72, 1), (9, 71, .5), (9.5, 69, .5), (10, 67, 1), (11, 69, 1),
     (12, 71, .5), (12.5, 71, .5), (13, 74, 1), (14, 76, 2)]
C = [(24, 76, 1), (25, 74, 1), (26, 71, 2), (28, 69, 1), (29, 67, 1), (30, 64, 2)]
lead = []
for off in (0, 32):
    lead += [(b + off, m, d) for b, m, d in A + B + C]
riff = [40, 43, 45, 47, 45, 43, 40, 38]
bass = []
for bar in range(16):
    if bar % 8 in (4, 5):
        bass += [(bar * 4 + i * .5, riff[i] + (2 if bar % 8 == 5 else 0), .5) for i in range(8)]
    else:
        root = [40, 36, 43, 38][bar % 4]
        bass += [(bar * 4 + k, root, 1) for k in range(4)]
chords = [(bar * 4, [[52, 55, 59], [48, 52, 55], [55, 59, 62], [50, 54, 57]][bar % 4], 4) for bar in range(16)]
score_song('Melody 110', 110, lead, bass, chords, 16, title='Melody Test', artist='The Q2 Band')

# Riff 140: a low guitar riff with fast repeated notes (double taps), the bass on the downbeats
riff16 = [(0, 52, .25), (.25, 52, .25), (.5, 55, .5), (1, 57, .25), (1.25, 57, .25), (1.5, 59, .5),
          (2, 62, .5), (2.5, 59, .25), (2.75, 59, .25), (3, 57, .5), (3.5, 55, .5)]
lead = [(bar * 4 + b, m + (0 if bar % 4 < 2 else 5), d) for bar in range(12) for b, m, d in riff16]
bass = [(bar * 4 + k * 2, 40 + (0 if bar % 4 < 2 else 5), 2) for bar in range(12) for k in range(2)]
score_song('Riff 140', 140, lead, bass, [], 12, hats16=True)

# Ballad 80: a slow, held, sung line over soft drums, a pad and a bass of whole notes
lead = []
tune = [(0, 67, 1.5), (1.5, 69, .5), (2, 71, 2), (4, 72, 1), (5, 71, 1), (6, 69, 2), (8, 67, 3), (11, 64, 1),
        (12, 65, 2), (14, 67, 2)]
for off in (0, 16):
    lead += [(b + off, m, d) for b, m, d in tune]
bass = [(bar * 4, [43, 45, 40, 41][bar % 4], 4) for bar in range(8)]
chords = [(bar * 4, [[55, 59, 62], [57, 60, 64], [52, 55, 59], [53, 57, 60]][bar % 4], 4) for bar in range(8)]
score_song('Ballad 80', 80, lead, bass, chords, 8, quiet_drums=True)

song('Rock 120', 120, 16, title='Test Rock', artist='The Q2 Band')
song('Shuffle 97', 97, 12, start=0.6, swing=0.3)
song('Fast 156', 156, 20, start=1.4, title='Fast Track — Ünïcödé', artist='Ωmega Бэнд')
print('made', sorted(os.listdir(out)))
