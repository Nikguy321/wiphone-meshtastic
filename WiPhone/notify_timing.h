/* notify_timing.h — how long a notification's one-shot sound is allowed to run.
 *
 * Header-only on purpose, like gnuboy/gb_romsize.h: WiPhone.ino cannot be compiled on the
 * Mac, so the arithmetic the phone ships is the arithmetic tests/test_notify.cpp checks.
 *
 * WHY THIS IS ARITHMETIC AND NOT A CONSTANT. The stop timer used to be a literal 280 ms next
 * to a comment saying the player "loops, so we stop it by timer after it has played through
 * once". pop_pcm is 5,120 bytes of 8 kHz 16-bit mono — 320 ms — so 280 never let it play
 * through. It was safe only because the sound is 720 samples (90 ms) of pop followed by
 * 230 ms of zeros (measured from the array, and pinned by the test), and cutting 40 ms of
 * silence is inaudible. Nobody knew that; a longer sample would have been cut mid-sound.
 *
 * THE TWO SOURCES NEED OPPOSITE MARGINS:
 *   - the SPIFFS file player wraps to byte 0 when the file runs out, and it pushes AHEAD of
 *     the DMA (4 x 1024 samples = 512 ms deep at 8 kHz), so by the time the first samples
 *     are audible the second iteration's attack — the loudest 40 ms of the sound — is already
 *     queued behind the tail. A stop that lands PAST the end replays it. Stop EARLY.
 *   - the memory source (Audio::playPcm) plays the array once and pads with silence, so a
 *     stop that lands late costs nothing and a stop that lands early cuts the tail. Stop LATE.
 * The margin is the same 40 ms either way; only its sign differs. */
#ifndef NOTIFY_TIMING_H
#define NOTIFY_TIMING_H

#include <stdint.h>
#include <stddef.h>

#define NOTIFY_POP_RATE_HZ           8000u   /* what Audio::playPop() configures the codec to */
#define NOTIFY_POP_BYTES_PER_SAMPLE  2u      /* 16-bit mono */
#define NOTIFY_POP_MARGIN_MS         40u

/* Playing time of a PCM buffer in ms, rounded UP so a timer derived from it never lands
 * before the last sample. */
static inline uint32_t notifyPcmMs(size_t bytes, uint32_t rateHz, uint32_t bytesPerSample) {
  if (rateHz == 0 || bytesPerSample == 0) {
    return 0;
  }
  const uint64_t samples = (uint64_t)bytes / bytesPerSample;
  return (uint32_t)((samples * 1000u + rateHz - 1) / rateHz);
}

/* How long after starting the one-shot to stop it, in ms. `sourceLoops` is true for the
 * file player (stop early) and false for the memory source (stop late). Never 0, and never
 * long enough for a second full play whatever the margin. */
static inline uint32_t notifyPopStopMs(size_t pcmBytes, uint32_t rateHz, uint32_t marginMs,
                                       bool sourceLoops) {
  const uint32_t len = notifyPcmMs(pcmBytes, rateHz, NOTIFY_POP_BYTES_PER_SAMPLE);
  if (len == 0) {
    return marginMs ? marginMs : 1;
  }
  if (sourceLoops) {
    return marginMs < len ? len - marginMs : len;
  }
  uint32_t stop = len + marginMs;
  if (stop >= 2 * len) {
    stop = 2 * len - 1;
  }
  return stop;
}

#endif // NOTIFY_TIMING_H
