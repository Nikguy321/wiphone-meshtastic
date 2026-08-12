# The music player

**Menu > Music.** MP3 and WAV, stereo through the headphone jack, shuffle and repeat, an
upload server of its own, and playback that keeps going when you leave the screen.

Built and flashed 2026-08-11. **Nobody has pressed play yet** — see the handoff.

## How it is put together

Four pieces, and the split is deliberate: everything that can be tested without the phone
is in a file that does not include Arduino.

| File | What it owns | Tested |
|---|---|---|
| `music_lib.{h,cpp}` | the track list and what plays next | 93 assertions, with `wav_reader` |
| `wav_reader.{h,cpp}` | WAV headers, downmix, resample | ″ |
| `mp3_stream.{h,cpp}` | ID3 skip, frame sync, feeding helix | 32 assertions, with the real decoder |
| `src/audio/helix-mp3/` | vendored decoder (RPSL) + our allocator | — |
| `Audio.cpp` | `playMusic()` and the two refill cases in `loop()` | on hardware |
| `music_player.{h,cpp}` | the library, queue and current track | on hardware |
| `app_music.{h,cpp}` | the screen | on hardware |

## The three things worth knowing before changing any of it

### 1. The player is not the app, and that is the point
Apps here are constructed when opened and **deleted when you back out**. If the library and
queue lived in `MusicApp`, leaving the screen would stop the music and forget your place.
So the state lives in `music_player.cpp` with static lifetime, ticked from the main loop
beside `meshService.loop()`, and the app is a view that can come and go.

### 2. The decoder must never allocate from the internal heap
helix wants ~29 KB in seven blocks. The internal heap has ~16–26 KB free depending on what
is up, and the failure is not a failed malloc — it **succeeds**, eats the margin, and some
minutes later the WiFi PHY cannot get 2 KB for RF calibration and `phy_init` calls
`abort()`, with a backtrace pointing nowhere near audio. That is the same bug opening a book
used to cause.

⚠ **The automatic rule does not save you here.** arduino-esp32 diverts large mallocs to
PSRAM, but that threshold is 16 KB and every one of helix's blocks is smaller — the biggest,
`IMDCTInfo`, is about 9 KB. Seven allocations that are individually small and collectively
fatal is exactly what it is blind to.

The fix is `src/audio/helix-mp3/helix_memory.c`: helix routes every allocation through
`helix_malloc`/`helix_free` and upstream only *declares* them, so we implement them against
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Explicit rather than `ps_malloc`, because
`ps_malloc` falls back to internal RAM when PSRAM is exhausted and refusing to play is much
better than rebooting later.

**Measured on the phone 2026-08-11:**
```
internal  26388 -> 26340   cost 48 bytes  (the Mp3Stream object itself)
largest   21952 -> 21952   unchanged, nothing fragmented
psram   3619660 -> 3591604 cost 28056 (27.4 KB)
after_free 26388           fully returned
```
Repeat it with `-DMP3_HEAP_PROBE` and read the boot log at 500000 baud.

### 3. Configure I2S from a DECODE, never from a header
A frame header can be read out of a false sync inside album art; a successful decode
cannot. Setting the clock from a bad header plays the whole track at the wrong speed and
nothing on the device says so. `playMusic()` decodes a frame first, keeps it (throwing it
away clips the start of every track), and configures the rate from what came out.

## Why WAV is here too, and why MP3 took the work it did

The phone's audio path was already built for stereo music: `configureI2S()` selects
`I2S_CHANNEL_FMT_RIGHT_LEFT` when `monoOut` is false, `playChunk()` has a direct interleaved
L/R case, the clock runs off the **APLL** so 44100 is exact, and `playDec[2400]` is exactly
1152 stereo frames — one MPEG-1 Layer III frame. That is not a coincidence. The vestigial
`Playback::LocalMp3` enum, the commented-out `playFile("/ringtone.mp3")` at `WiPhone.ino:93`
and "TODO: migrate to dr_mp3" atop `Audio.h` are the rest of an MP3 player WiPhone intended
and never finished. It was only ever the allocator in the way.

WAV shipped first as the guaranteed path and stayed because it costs nothing: `dr_wav` was
already vendored, decoding is a memcpy, and it is a useful fallback if a file will not
decode.

## Traps

1. **`playFile()` loops forever** — it is the ringtone player and rewinds at EOF. Music uses
   `playMusic()`. `musicEnded()` waits for the decoded samples to *drain*, not just the file
   to end, or every track is clipped.
2. **Format comes from content, not extension.** The uploader has no extension filter — a
   `.wav` holding an MP3 is ordinary input.
3. **WAV `dataBytes` is a lie in streamed files** (`0xFFFFFFFF` or 0). Clamp to the real file
   size or playback runs off the end.
4. **ID3v2 sizes are syncsafe** — 7 bits per byte. Reading them big-endian is wrong for every
   tag over 128 bytes, i.e. all of them, and the result is seeking past the start of the song.
5. **End-of-file and the Next key are different operations.** Repeat-one replays at EOF but
   Next must still move, or you are trapped on one track.
6. **A rescan reorders the library**, so what is playing is remembered by path, not index.
7. **`ControlState::inputType` is a mode that persists across screens.** The now-playing
   screen forces Numeric, or 4 and 6 arrive as 'g' and 'm' depending on which app you opened
   earlier.
8. **helix is C.** It must not go through the C++ front end — C++11 narrowing rejects its
   constant tables. The host harness builds it with `$CC`, ASan on, UBSan off (it shifts
   negative values left all over the fixed-point DSP, which is what it means to do).

## Licensing

helix is **RealNetworks RPSL**, not MIT. Open source and satisfied by this repo being
public, but worth knowing before reusing any of it. The popular Arduino wrapper around it is
**GPLv3**, which is why only the raw sources are vendored and the glue is ours.

## Not built

- **Seeking.** No scrub bar, and Pause/Resume restarts the track. Seeking a VBR MP3 means
  either indexing the whole file or landing in the wrong place.
- **Tags.** The list shows filenames. Reading ID3 title/artist per track would mean opening
  every file to draw the library, which is the same reason Books shows no progress column.
- **Gapless playback.** There is a gap between tracks while the next file opens.
- **A decode-ahead ring buffer.** Designed for in `mp3_stream.h`, only needed if playback
  stutters. Try it before a second task on core 0.
