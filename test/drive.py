#!/usr/bin/env python3
"""Runs the host build of q2-hero as Q2 Pod would (fake /dev/fb0 from fbshim.so, input as text
datagrams on /tmp/q2-hero.input.sock, TERM to end it) through one scenario, on a fake card
holding test/songs, with the host's ffmpeg. Prints the log, the last state and results; frames
are saved as OUT_DIR/<scenario>-<name>.raw (view.py shows them).

The bot plays the notes the game wrote to chart.txt, pressing each at its time on the game's own
clock (state.txt's now_ms and now_us, CLOCK_MONOTONIC as Python's), through the socket.

Usage: drive.py BUILD_DIR OUT_DIR SCENARIO
Scenarios: menus (title, songs, listen, difficulty, a whole song played well, results, the kept
chart), miss (a song left alone: it fails), pause, options (options and the timing check), lefty"""
import os, shutil, signal, socket, subprocess, sys, tempfile, threading, time

build, outdir, scenario = sys.argv[1:4]
build, outdir = os.path.abspath(build), os.path.abspath(outdir)
here = os.path.dirname(os.path.abspath(__file__))
SOCK = '/tmp/q2-hero.input.sock'
CENTRE, PLAY, PREV, NEXT, RETURN = 218, 171, 222, 223, 170
LANE_KEY = [PREV, CENTRE, NEXT]

card = tempfile.mkdtemp()
app = os.path.join(card, '.hero')
os.makedirs(app)
music = os.path.join(card, 'Music', 'Test Band')
os.makedirs(music)
for f in ('Rock 120.mp3', 'Fast 156.mp3', 'Shuffle 97.flac'):
    shutil.copy(os.path.join(here, 'songs', f), music)
os.makedirs(os.path.join(card, 'Podcasts'))
jp = os.path.join(card, 'Music', 'J-Pop 日本')  # names from tags, and a Japanese file name with none
os.makedirs(jp)
for f in ('id3v23.mp3', 'jp.flac', 'jp.m4a', 'jp.opus', 'latin.mp3', '千本桜.mp3'):
    shutil.copy(os.path.join(here, 'tagsongs', f), jp)
shutil.copy(os.path.join(build, 'q2-hero'), app)
caps = os.path.join(card, 'caps')
open(caps, 'w').write('game audio volume dim\n')
dump = os.path.join(card, 'fb.raw')
env = dict(os.environ, LD_PRELOAD=os.path.join(build, 'fbshim.so'), FB_DUMP=dump, FB_FINAL=os.path.join(card, 'fb.final'),
           HERO_STATE='1', HERO_MUSIC=card, HERO_SYSFONT='/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf', HERO_FFMPEG=shutil.which('ffmpeg'), HERO_CAPS=caps,
           FAKE_PCM_DUMP=os.path.join(card, 'pcm.raw'), FAKE_PCM_ONSETS=os.path.join(card, 'onsets.txt'))
if scenario == 'menus':
    env['HERO_SLOW_LISTEN'] = '1'
if os.path.exists(SOCK):
    os.unlink(SOCK)
if scenario == 'lefty':
    open(os.path.join(app, 'settings.txt'), 'w').write('lefty 1\nspeed 4\n')
proc = subprocess.Popen([os.path.join(app, 'q2-hero'), 'plughw:0,0'], env=env, cwd=app)
log = os.path.join(app, 'hero.log')

def wait_for(cond, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if cond():
            return
        time.sleep(0.02)
    proc.kill()
    sys.exit(f'FAIL: no {what} after {secs} s (state {state()})\n' + (open(log).read() if os.path.exists(log) else ''))

def state():
    out = {}
    try:
        for line in open(os.path.join(app, 'state.txt')):
            k, v = line.split()
            out[k] = int(v)
    except (OSError, ValueError):
        pass
    return out

S_TITLE, S_BROWSE, S_LISTEN, S_SONG, S_PLAY, S_PAUSE, S_RESULTS, S_OPTIONS, S_CALIBRATE, S_NOTE, S_VOLUME = range(11)
wait_for(lambda: os.path.exists(SOCK), 5, 'input socket')
wait_for(lambda: state().get('state') == S_TITLE, 10, 'first frame')
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

def send(m):
    s.sendto(m.encode(), SOCK)

def key(code, hold=0.05, gap=0.12):
    send(f'k d {code}'); time.sleep(hold); send(f'k u {code}'); time.sleep(gap)

def wheel(total, steps=4, gap=0.03):
    for _ in range(steps):
        send(f'w {total // steps}'); time.sleep(gap)

def tap(x, y):
    send(f't d {x} {y}'); time.sleep(0.05); send(f't u {x} {y}'); time.sleep(0.15)

def snap(name):
    time.sleep(0.1)
    for _ in range(50):  # fbshim rewrites the file each frame: take a whole one
        data = open(dump, 'rb').read()
        if len(data) == 320 * 375 * 4:
            break
        time.sleep(0.01)
    open(os.path.join(outdir, f'{scenario}-{name}.raw'), 'wb').write(data)

def wait_state(st, secs):
    wait_for(lambda: state().get('state') == st, secs, f'state {st}')

def open_song(index, diff_steps):
    """from the title: Play, into Music/Test Band, the song at index, a difficulty"""
    key(CENTRE); wait_state(S_BROWSE, 3)
    key(CENTRE); time.sleep(0.2)            # Music (first: folders sort first)
    wheel(-20, 1); time.sleep(0.1)          # past J-Pop 日本
    key(CENTRE); time.sleep(0.2)            # Test Band
    for _ in range(index):
        wheel(-20, 1); time.sleep(0.05)
    key(CENTRE)
    wait_for(lambda: state().get('state') in (S_LISTEN, S_SONG), 5, 'listen or song')
    wait_state(S_SONG, 60)
    time.sleep(0.3)
    for _ in range(abs(diff_steps)):
        key(NEXT if diff_steps > 0 else PREV)

def read_chart():
    notes = []
    for line in open(os.path.join(app, 'chart.txt')):
        t, lanes, ln, hero = line.split()
        notes.append((float(t), int(lanes), float(ln), int(hero)))
    return notes

def game_clock():
    """game seconds -> monotonic seconds, from a few frames' readings"""
    offs = []
    for _ in range(10):
        st = state()
        if st.get('now_us'):
            offs.append(st['now_us'] / 1e6 - st['now_ms'] / 1000)
        time.sleep(0.03)
    offs.sort()
    return offs[len(offs) // 2]

def bot(notes, skill=1.0, stop=None, hero_at=None, touch=False, lefty=False):
    """press every note at its time (a share of them, skill), holding held notes; or touch the
    lane on the screen (one finger: a chord's second lane can't be touched)"""
    base = game_clock()
    events = []
    for i, (t, lanes, ln, hero) in enumerate(notes):
        if (i * 7919 % 100) >= skill * 100:
            continue
        for l in range(3):
            if not lanes >> l & 1:
                continue
            if touch:
                x = 187 + ((2 - l if lefty else l) - 1) * 75
                events.append((t, f't d {x} 270'))
                events.append((t + (ln if ln > 0 else 0.05), f't u {x} 270'))
                break
            events.append((t, f'k d {LANE_KEY[l]}'))
            events.append((t + (ln if ln > 0 else 0.06), f'k u {LANE_KEY[l]}'))
    events.sort()
    for t, m in events:
        if stop and stop():
            return
        wait = base + t - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        send(m)
        if hero_at and t > hero_at[0] and not hero_at[1]:
            hero_at[1] = True
            send(f'k d {PLAY}'); send(f'k u {PLAY}')

results = {}
if scenario == 'menus':
    snap('title')
    key(CENTRE); wait_state(S_BROWSE, 3); snap('songs-root')
    results['root_entries'] = state()['nents']
    key(CENTRE); time.sleep(0.2); wheel(-20, 1); time.sleep(0.1); key(CENTRE); time.sleep(0.3); snap('songs-band')
    results['band_entries'] = state()['nents']
    wheel(-20, 1); time.sleep(0.1)  # Rock 120.mp3 (Fast, Rock, Shuffle)
    key(CENTRE)
    wait_for(lambda: state().get('state') == S_LISTEN, 5, 'listening')
    time.sleep(1.0); snap('listening')
    wait_state(S_SONG, 60); time.sleep(0.3); snap('song')
    results['bpm10'] = state()['bpm10']
    key(NEXT); time.sleep(0.2)  # Hard
    results['diff'] = state()['diff']
    key(CENTRE); wait_state(S_PLAY, 5)
    time.sleep(0.5); snap('lead-in')
    notes = read_chart()
    results['chart_notes'] = len(notes)
    snapper = threading.Thread(target=lambda: [time.sleep(t) or snap(n) for t, n in ((4.5, 'playing'), (7, 'playing-2'))])
    snapper.start()
    # a held note with a note under it, coming down: when it is half a second off
    under = [n for n in notes if n[2] > 0 and any(n[0] < m[0] < n[0] + n[2] for m in notes)]
    results['held_with_under'] = len(under)
    if under:
        def snap_hold(at=under[0][0] - 0.45):
            base = game_clock()
            time.sleep(max(0, base + at - time.monotonic())); snap('playing-held')
        threading.Thread(target=snap_hold).start()
    bot(notes, hero_at=[20.0, False])
    snapper.join()
    wait_state(S_RESULTS, 30); time.sleep(2.2); snap('results')
    st = state()
    results['hits'], results['notes'], results['misses'], results['score'] = st['hits'], st['notes'], st['misses'], st['score']
    results['stars'], results['best_streak'] = st['stars'], st['best_streak']
    key(NEXT); key(CENTRE); wait_state(S_SONG, 3); time.sleep(0.3); snap('song-after')
    key(RETURN); wait_state(S_BROWSE, 3); time.sleep(0.2); snap('songs-starred')
    # the same song again: kept, so no listening
    key(CENTRE); time.sleep(0.25)
    results['second_open'] = state()['state']
    results['charts_kept'] = len(os.listdir(os.path.join(app, 'charts')))
elif scenario == 'names':
    key(CENTRE); wait_state(S_BROWSE, 3)
    key(CENTRE); time.sleep(0.3); snap('music')  # J-Pop 日本 and Test Band
    results['music_entries'] = state()['nents']
    key(CENTRE); time.sleep(1.0); snap('jpop')
    results['jpop_entries'] = state()['nents']
    results['tags_cached'] = sum(1 for _ in open(os.path.join(app, 'tags.txt'))) if os.path.exists(os.path.join(app, 'tags.txt')) else 0
    key(CENTRE)  # the first song: its title from its tags
    wait_state(S_SONG, 30); time.sleep(0.3); snap('song')
elif scenario == 'miss':
    open_song(0, 0)  # Fast 156, Medium
    key(CENTRE); wait_state(S_PLAY, 5)
    wait_for(lambda: state().get('failed') == 1, 60, 'a fail')
    time.sleep(0.6); snap('failing')
    wait_state(S_RESULTS, 10); time.sleep(1.0); snap('failed')
    st = state()
    results['failed'], results['stars'], results['hits'] = st['failed'], st['stars'], st['hits']
elif scenario == 'pause':
    open_song(1, -1)  # Rock 120, Easy
    key(CENTRE); wait_state(S_PLAY, 5)
    time.sleep(4.0)
    t0 = state()['now_ms']
    send(f'k d {RETURN}')
    for _ in range(6):  # held: Q2 Pod passes its repeats on as more downs
        time.sleep(0.07); send(f'k d {RETURN}')
    send(f'k u {RETURN}'); time.sleep(0.3)
    results['paused'] = state()['state']
    t1 = state()['now_ms']; time.sleep(1.0); t2 = state()['now_ms']
    results['clock_moved_ms'] = t2 - t1
    snap('paused')
    key(CENTRE); time.sleep(0.6); snap('counting')
    time.sleep(1.4)
    results['resumed'] = state()['state']
    t3 = state()['now_ms']; time.sleep(1.0); t4 = state()['now_ms']
    results['clock_rate'] = t4 - t3
    key(RETURN); time.sleep(0.3); wheel(-40, 2); time.sleep(0.2); key(CENTRE); time.sleep(0.4)
    results['quit_to'] = state()['state']
    # the volume: the wheel while playing leaves it alone (the buttons sit on the wheel); the pause
    # menu's speaker opens the volume screen, where the wheel and the bar set it
    def volume_file():
        try:
            return open('/tmp/q2-hero.volume').read().strip()
        except OSError:
            return 'none'
    key(CENTRE); wait_state(S_PLAY, 5); time.sleep(3.0)
    send('v 40'); time.sleep(0.2)
    before = volume_file()
    wheel(-60, 6); time.sleep(0.3)
    results['play_wheel_left_volume'] = volume_file() == before
    key(RETURN); wait_state(S_PAUSE, 3)
    wheel(-60, 3); time.sleep(0.2); snap('speaker')
    key(CENTRE); wait_state(S_VOLUME, 3)
    wheel(-60, 6); time.sleep(0.3); snap('volume')
    results['volume_by_wheel'] = volume_file()
    send('t d 188 214'); time.sleep(0.1); send('t m 245 214'); time.sleep(0.1); send('t u 245 214'); time.sleep(0.3)
    results['volume_by_bar'] = volume_file()
    key(CENTRE); time.sleep(0.2)
    results['volume_back_to'] = state()['state']
    tap(187, 266); time.sleep(0.2)  # the speaker, tapped
    results['speaker_tapped'] = state()['state']
    key(RETURN); time.sleep(0.2); key(RETURN); time.sleep(2.0)  # back to the pause menu, then resume
    results['resumed_after_volume'] = state()['state']
elif scenario == 'options':
    wheel(-20, 1); key(CENTRE); wait_state(S_OPTIONS, 3); snap('options')
    key(CENTRE); time.sleep(0.1)  # speed 3 -> 4
    wheel(-20, 1); key(CENTRE)    # lefty on
    wheel(-60, 3); time.sleep(0.1)  # Timing
    key(CENTRE); wheel(-40, 4); time.sleep(0.2); snap('timing-edit')
    key(CENTRE)
    results['offset_set'] = state()['offset']
    wheel(-20, 1); key(CENTRE); wait_state(S_CALIBRATE, 3)
    # press 40 ms after each click is heard: the clicks are every 0.5 s of the clock from 0
    base = game_clock()
    time.sleep(0.4); snap('calibrate')
    for k in range(2, 20):
        wait = base + k * 0.5 + 0.040 - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        send(f'k d {CENTRE}'); send(f'k u {CENTRE}')
        if state().get('cal_taps', 0) >= 16:
            break
    time.sleep(0.5); snap('calibrated')
    # when the device played each click, against when the game's clock says it is heard
    heard = [float(l) for l in open(os.path.join(card, 'onsets.txt'))]
    errs = []
    for k in range(0, 20):
        want = base + k * 0.5
        near = min(heard, key=lambda h: abs(h - want)) if heard else None
        if near is not None and abs(near - want) < 0.2:
            errs.append(near - want)
    results['clicks_heard'] = len(errs)
    results['onsets_logged'] = len(heard)
    results['first_onsets'] = [round(h - base, 3) for h in heard[:6]]
    results['click_vs_clock_ms'] = round(1000 * sum(errs) / len(errs), 1) if errs else 'none'
    results['cal_ms'] = state()['cal_ms']
    key(CENTRE); time.sleep(0.2)
    results['offset_saved'] = state()['offset']
    results['speed'] = state()['speed']
    key(RETURN); wait_state(S_TITLE, 3)
    results['settings'] = open(os.path.join(app, 'settings.txt')).read().replace('\n', ' ')
elif scenario == 'lefty':
    open_song(2, 1)  # Shuffle 97, Hard
    key(CENTRE); wait_state(S_PLAY, 5)
    notes = read_chart()
    threading.Thread(target=lambda: (time.sleep(5), snap('playing'))).start()
    bot(notes, touch=True, lefty=True)
    wait_state(S_RESULTS, 30); time.sleep(1.5); snap('results')
    st = state()
    results['hits'], results['notes'] = st['hits'], st['notes']
    results['chords'] = sum(1 for n in notes if n[1] == 5)

end = state()
proc.send_signal(signal.SIGTERM)
try:
    status = proc.wait(5)
except subprocess.TimeoutExpired:
    proc.kill()
    status = 'killed'
print(open(log).read())
print('exit status:', status)
print('panel:', open(os.path.join(card, 'fb.final')).read().strip() if os.path.exists(os.path.join(card, 'fb.final')) else '-')
print('socket left:', os.path.exists(SOCK))
for k, v in end.items():
    print(f'end: {k}: {v}')
for k, v in results.items():
    print(f'result: {k}: {v}')
pcm = os.path.join(card, 'pcm.raw')
if os.path.exists(pcm):
    shutil.copy(pcm, os.path.join(outdir, f'{scenario}-pcm.raw'))
shutil.rmtree(card)
