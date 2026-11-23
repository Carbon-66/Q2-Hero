Q2 HERO for the Shanling Q2 (Q2 Pod)
====================================

A guitar game played to the music on your card. Pick any song, pick a difficulty, and hit the
notes as they come down three lanes, with Prev, Centre and Next.

INSTALL
1. Copy the .hero folder to the root of the card. On a Mac, press Cmd-Shift-. in Finder to see
   dot-folders.
2. Eject the card and open Library -> Games -> Q2 Hero.

It plays through whatever Q2 Pod plays through: the headphone jack, Bluetooth or a USB DAC.

PLAYING
Choose Play, then a song from the card's folders, then a difficulty.

The first time you pick a song, Q2 Hero listens to it once to find its beat and what sounds on and
between the beats: the drums, the tune and the bass. The notes are charted from that, the way
people chart rhythm games by hand. This takes a few seconds (a progress bar shows it);
after that the chart is kept in .hero/charts and the song opens straight away.

  Prev / Centre / Next   the left, middle and right lanes: press as a note crosses its ring
  Tap the lane           the same, on the touchscreen (one tap plays a chord too)
  Hold the button        on a note with a tail, until the tail ends
  Prev + Next together   a chord (a joined pair of notes)
  Play/Pause             Hero Power, once its meter is half full
  Return                 pause: Resume, Restart, Quit Song, or the speaker for the volume
  Hold Return            leave the game (Q2 Pod's own)

The wheel does nothing while you play, as the buttons sit on it. For the volume, pause and pick
the speaker (or tap it): turn the wheel or slide the bar, then Centre.

In the menus the wheel (or Prev and Next) moves, Centre picks, Return goes back, and you can tap.

SCORING
Each note is 50 points times your multiplier. Ten notes in a row is 2x, twenty is 3x, thirty is
4x. A miss, or a press with no note to hit, ends the streak. Held notes score while you hold them.

The star notes are a Hero phrase: hit all of them and a quarter of the Hero meter fills. From half
full, Play/Pause doubles the multiplier until the meter runs out.

Misses drain the Rock meter on the left. If it empties, the song fails (Options -> Fail: No Fail
turns that off). Stars at the end go by score, five at best. Your best on each song and difficulty
shows in the song list.

DIFFICULTIES
  Easy     the strongest beats; long sung and string notes held (about half the notes)
  Medium   nearly every beat, the off-beats where the music hits them, a few chords, many holds
  Hard     the beats and most off-beats, chords on strong beats; hold Prev or Next while
           tapping the other side
  Expert   the off-beats in full, fills where the music is busy, many chords

No difficulty asks for one button to be pressed faster than about four times a second, or
three times in a row quickly: fast passages move between the buttons instead.

Where someone sings, the notes follow the singing first; where no one does, the main tune; and
where there's no tune, the bass. The drums keep the pulse in between. The notes sit on the beat,
like a hand-made chart: a part of the song that comes back plays the
same rhythm again. Where the singer, the lead or the strings hold a note, you hold one. The lanes
move in shapes (a bounce, a zigzag, steps, an anchor on Centre), picked to rise and fall with the
tune where it can be heard. Swing songs are charted on the swing.

Most songs keep one steady tempo, and Q2 Hero finds it and lays the notes on it exactly, as a
charter would. Quiet parts of a song play gently. Q2 Hero hears the tune best in stereo songs,
where the voice sits in the middle. If every note still feels early or late, run Check timing in
Options: it measures your Q2's own delay.

Charts made by an older Q2 Hero are made again the first time each song is picked.

OPTIONS
  Note speed     how fast the notes come down (1 to 5)
  Lefty flip     the lanes mirrored: Next on the left, Prev on the right
  Fail           On, or No Fail
  Effects        the game's own sounds: High, Low or Off
  Timing         how late your presses count, in ms
  Check timing   press Centre along to 16 clicks and Q2 Hero sets Timing for you

Timing is kept for each output separately: the jack, Bluetooth and USB. Bluetooth headphones lag
behind the sound, so if the notes feel late there, run Check timing with them on.

GOOD TO KNOW
- Any format the Q2 plays works: MP3, FLAC, AAC/M4A, ALAC, Ogg, Opus, WAV, AIFF, APE and more.
- Songs show by the title in their tags, with the artist beside it, or by their file name when
  they have none. Names in any script the Q2 shows (Japanese, Chinese, Korean...) show as the
  Q2's own screens draw them. The tags are kept in .hero/tags.txt, so a folder opens at once the
  next time.
- Left on the menus for five minutes, Q2 Hero closes itself.
- Songs aren't changed. To make Q2 Hero listen to a song again, delete .hero/charts.
- From 1.0: Q2 Hero 1.1 listens to each song again the first time you pick it, as it hears more.
- .hero/hero.log says what happened on the last run, if something goes wrong.
