#!/bin/sh
# Short songs tagged in every format tags.c reads, a Japanese title and artist in most, for
# test/run.sh: test/tagsongs/. Needs ffmpeg with libmp3lame, libopus, libvorbis and wavpack.
set -e
cd "$(dirname "$0")"
mkdir -p tagsongs
SRC="songs/Rock 120.mp3"; T="夜に駆ける"; A="YOASOBI・ヨアソビ"
f() { out=$1; shift; ffmpeg -v error -y -i "$SRC" -t 3 "$@" "tagsongs/$out"; }
f id3v24.mp3 -metadata title="$T" -metadata artist="$A" -c:a libmp3lame
f id3v23.mp3 -metadata title="$T" -metadata artist="$A" -id3v2_version 3 -c:a libmp3lame
f latin.mp3 -metadata title="Café Déjà Vu" -metadata artist="Éric" -id3v2_version 3 -c:a libmp3lame
f jp.flac -metadata title="$T" -metadata artist="$A"
f jp.m4a -metadata title="$T" -metadata artist="$A" -c:a aac
f alac.m4a -metadata title="$T" -metadata artist="$A" -c:a alac
f jp.opus -metadata title="$T" -metadata artist="$A" -c:a libopus
f jp.ogg -metadata title="$T" -metadata artist="$A" -c:a libvorbis
f jp.wav -metadata title="Wave Title" -metadata artist="Wave Artist"
f jp.aiff -metadata title="$T" -metadata artist="$A" -write_id3v2 1
f jp.wv -metadata title="$T" -metadata artist="$A" -c:a wavpack
f 千本桜.mp3 -map_metadata -1 -c:a libmp3lame
