#!/bin/sh
# Host tests for q2-hero (x86-64 Linux: clang, python3 with numpy and Pillow, ffmpeg):
#   1. test songs with known beats (mksongs.py), listened to and charted by listen_tool, and
#      checked against their truth: the tempo, the beats and hits within a few ms, every
#      difficulty's notes on real hits and on the grid, lanes, repeating bars (accuracy.py);
#   2. the real game and platform layer on fbshim.so's fake /dev/fb0 and alsa_fake.c's PCM,
#      driven through Q2 Pod's input socket (drive.py), a bot playing the charts on the game's
#      clock. Each scenario must end on TERM with status 0, the panel back on Q2 Pod's page and
#      the socket gone. Frames land in test/out/*.raw and .png.
set -e
cd "$(dirname "$0")/.."
export HERO_FFMPEG="$(command -v ffmpeg)"
mkdir -p test/out
[ -f "test/songs/Rock 120.truth" ] || python3 -I test/mksongs.py test/songs > /dev/null
[ -f "test/tagsongs/jp.wv" ] || ./test/mktagsongs.sh
# the tags of every format, then the same files broken every which way (no crash, no bad read)
cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all test/tags_tool.c src/tags.c -o build/host/tags_tool 2>/dev/null ||
    cc -O1 -g test/tags_tool.c src/tags.c -o build/host/tags_tool
./build/host/tags_tool test/tagsongs/* | sed 's|test/tagsongs/||' > test/out/tags.txt
for f in id3v24.mp3 id3v23.mp3 jp.flac jp.m4a alac.m4a jp.opus jp.ogg jp.aiff jp.wv; do
    grep -q "^$f	1	夜に駆ける	YOASOBI・ヨアソビ$" test/out/tags.txt || { echo "FAIL: tags of $f"; cat test/out/tags.txt; exit 1; }
done
grep -q "^latin.mp3	1	Café Déjà Vu	Éric$" test/out/tags.txt && grep -q "^jp.wav	1	Wave Title	Wave Artist$" test/out/tags.txt &&
    grep -q "^千本桜.mp3	0		$" test/out/tags.txt || { echo "FAIL: tags"; cat test/out/tags.txt; exit 1; }
mkdir -p test/out/fuzz
python3 -I -c "
import os, random
random.seed(1)
n = 0
for f in sorted(os.listdir('test/tagsongs')):
    data = open('test/tagsongs/' + f, 'rb').read()
    for k in range(40):
        d = bytearray(data[:random.choice([10, 50, 200, 1000, 5000, len(data)])])
        for _ in range(random.randint(0, 30)):
            if d: d[random.randrange(len(d))] = random.randrange(256)
        open('test/out/fuzz/%d.bin' % n, 'wb').write(bytes(d)); n += 1
"
./build/host/tags_tool test/out/fuzz/*.bin > /dev/null 2> test/out/fuzz.err || { echo "FAIL: tags on broken files"; head test/out/fuzz.err; exit 1; }
echo "tags: ok (ID3v2.3, 2.4, FLAC, MP4/AAC, ALAC, Opus, Vorbis, AIFF, WavPack/APE, WAV; 480 broken files)"
./test/hostbuild.sh
clang -O2 -fno-math-errno -w -pthread test/listen_tool.c src/analyze.c src/chart.c src/decode.c src/gfx.c src/fontdata.c src/logo.c src/sysfont.c src/tags.c -o build/host/listen_tool
for s in "Rock 120.mp3" "Shuffle 97.flac" "Fast 156.mp3" "Melody 110.mp3" "Riff 140.mp3" "Ballad 80.mp3"; do
    ./build/host/listen_tool "test/songs/$s" > test/out/listen.txt 2>/dev/null
    python3 -I test/accuracy.py test/out/listen.txt "test/songs/${s%.*}.truth" > test/out/accuracy.txt ||
        { echo "FAIL: charting $s"; cat test/out/accuracy.txt; exit 1; }
    echo "chart $s: $(head -3 test/out/accuracy.txt | tr '\n' ' ')"
    grep "_shape" test/out/accuracy.txt | sed 's/^/    /' || true
done
for sc in menus names miss pause options lefty; do
    timeout 300 python3 -I test/drive.py build/host test/out $sc > test/out/$sc.log 2>&1 || { cat test/out/$sc.log; exit 1; }
    grep -q "^exit status: 0" test/out/$sc.log || { echo "FAIL: $sc: exit status"; cat test/out/$sc.log; exit 1; }
    grep -q "^panel: yoffset 0 " test/out/$sc.log || { echo "FAIL: $sc: panel not handed back"; exit 1; }
    grep -q "^socket left: False" test/out/$sc.log || { echo "FAIL: $sc: socket left behind"; exit 1; }
    echo "$sc: ok"
done
res() { grep "^result: $2:" test/out/$1.log | sed 's/^result: [a-z_0-9]*: //'; }
[ "$(res menus root_entries)" = 2 ] && [ "$(res menus band_entries)" = 3 ] || { echo "FAIL: songs: the card's folders and songs"; exit 1; }
[ "$(res menus bpm10)" -ge 1190 ] && [ "$(res menus bpm10)" -le 1210 ] || { echo "FAIL: listen: tempo $(res menus bpm10)"; exit 1; }
[ "$(res menus hits)" = "$(res menus notes)" ] && [ "$(res menus misses)" = 0 ] || { echo "FAIL: play: the bot missed notes on the keys"; exit 1; }
[ "$(res menus held_with_under)" -ge 1 ] || { echo "FAIL: Hard has no held note with notes under it"; exit 1; }
[ "$(res menus stars)" -ge 4 ] || { echo "FAIL: play: a perfect game earned $(res menus stars) stars"; exit 1; }
[ "$(res menus second_open)" = 3 ] && [ "$(res menus charts_kept)" = 1 ] || { echo "FAIL: the kept chart wasn't used"; exit 1; }
[ "$(res miss failed)" = 1 ] && [ "$(res miss stars)" = 0 ] || { echo "FAIL: a song left alone didn't fail"; exit 1; }
[ "$(res pause paused)" = 5 ] && [ "$(res pause clock_moved_ms)" -lt 20 ] && [ "$(res pause resumed)" = 4 ] ||
    { echo "FAIL: pause: held Return, the clock stopped, Resume"; exit 1; }
r=$(res pause clock_rate); [ "$r" -gt 950 ] && [ "$r" -lt 1050 ] || { echo "FAIL: the clock ran at $r ms a second"; exit 1; }
[ "$(res pause quit_to)" = 3 ] || { echo "FAIL: pause: Quit Song"; exit 1; }
[ "$(res pause play_wheel_left_volume)" = True ] || { echo "FAIL: the wheel changed the volume while playing"; exit 1; }
[ "$(res pause volume_by_wheel)" = 50 ] && [ "$(res pause volume_by_bar)" = 75 ] && [ "$(res pause volume_back_to)" = 5 ] &&
    [ "$(res pause speaker_tapped)" = 10 ] && [ "$(res pause resumed_after_volume)" = 4 ] ||
    { echo "FAIL: the volume screen: wheel $(res pause volume_by_wheel), bar $(res pause volume_by_bar)"; exit 1; }
c=$(res options cal_ms); [ "$c" -ge 30 ] && [ "$c" -le 50 ] && [ "$(res options offset_saved)" = "$c" ] ||
    { echo "FAIL: the timing check measured $c ms for presses 40 ms late"; exit 1; }
k=$(res options click_vs_clock_ms | sed 's/\..*//; s/-//'); [ "$k" -le 5 ] || { echo "FAIL: clicks played $(res options click_vs_clock_ms) ms off the game's clock"; exit 1; }
[ "$(res names music_entries)" = 2 ] && [ "$(res names jpop_entries)" = 6 ] && [ "$(res names tags_cached)" = 6 ] ||
    { echo "FAIL: names: the Japanese folder, its songs, the tags kept"; exit 1; }
[ "$(res lefty hits)" = "$(res lefty notes)" ] || { echo "FAIL: lefty, by touch: $(res lefty hits)/$(res lefty notes)"; exit 1; }
for f in test/out/*.raw; do case "$f" in *-pcm.raw) continue;; esac; python3 -I test/view.py "$f" "${f%.raw}.png"; done
echo "all ok: charts on the beat, a whole song played on the keys and by touch, Japanese names and tags, fail, pause, the volume screen, timing check, clock against the speaker"
