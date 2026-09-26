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
/* The ring playPop() plays on: Audio::installI2S()'s DEFAULT geometry, mono, at the rate above.
 * For the test's worked numbers only - the phone asks the driver what is installed. */
#define NOTIFY_POP_RING_BUFS         4u
#define NOTIFY_POP_RING_BUF_SAMPLES  1024u

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

/* ── 🛑 THE RING'S LEAD: A FRESH RING PLAYS ITSELF ONCE BEFORE ANYTHING WRITTEN IS HEARD ──────
 * (integration review 3, A1.) Everything above times the SOUND; this times the DMA ring in front
 * of it. IDF 3.3's i2s_create_dma_queue() builds the TX free-buffer queue EMPTY (checked in the
 * libdriver.a this build links: it calls xQueueGenericCreate and nothing else), and neither
 * i2s_set_clk() nor i2s_start() fills it. So on a ring that is freshly installed, or installed and
 * then started, i2s_write() with timeout 0 takes NOTHING until the first buffer has played out
 * and its end-of-buffer interrupt hands it back: buffer 0 plays zeros, comes back after one
 * buffer's time, gets the first samples, and those reach the DAC one whole trip after the start.
 * music_feed.h and test_musicfeed's DmaModel say the same ("a freshly installed ring plays itself
 * (zeros) once before anything written reaches the DAC").
 *
 * The pop's ring is 4 x 1024 mono at 8 kHz = 512 ms a trip, and playPop() installs it FRESH on
 * nearly every pop (the boot ring is 16 kHz stereo; music's and a game's are other rings; and the
 * last pop's restore() put one of those back). The chirp (the first 90 ms of pop_pcm) was due at
 * the DAC at ~512 ms - and the teardown at 360 ms zeroed every buffer (ceasePlayback) and
 * reinstalled the ring (restore()), 150 ms before it could sound. It survived only when a loop
 * stall pushed the teardown past ~512 ms: the 0.9.65 report "sometimes sound will play, but
 * sometimes it won't". 0.9.66's exact stop timer and 0.9.79's non-blocking LoRa send took most of
 * those stalls away, and the master mute hid the rest. (Before 0.9.66 the SPIFFS open, 1.2-1.6 s,
 * sat between the start and the first write, so the queue was full and the chirp played at once.)
 *
 * HOW THE POP FOUND THE RING decides the lead (Audio::playPop() reports it):
 *   FRESH     - installed for this pop (or its rate changed, which forces a reinstall): the queue
 *               is empty and the DMA at buffer 0, so the lead is EXACTLY one trip.
 *   RESTARTED - the device was off and the ring it left is reused: start()'s i2s_start() puts the
 *               DMA back at buffer 0 under the queue as the last session left it, so the first
 *               buffer written is somewhere up to three buffers ahead of the DMA - a lead anywhere
 *               in [0, one trip).
 *   RUNNING   - the device was on and the ring kept: the queue is full, the first write lands in
 *               the buffer playing now or the next. No lead.
 * A range is resolved by the source's own margin rule above: the play-once memory source must
 * stop LATE, so it takes the whole trip (only zero padding is lengthened); the looping file must
 * stop EARLY, so it takes none (its second attack is never replayed; the chirp may be cut, as it
 * always could be). FRESH is exact, so both take it. */
enum NotifyRing {
  NOTIFY_RING_RUNNING   = 0,
  NOTIFY_RING_RESTARTED = 1,
  NOTIFY_RING_FRESH     = 2,
};

/* One trip round a DMA ring of `bufs` buffers of `bufSamples` frames at `rateHz`, in ms, rounded
 * UP. 0 for a ring that is not there (no driver: the caller refused anyway). */
static inline uint32_t notifyRingTripMs(uint32_t bufs, uint32_t bufSamples, uint32_t rateHz) {
  if (bufs == 0 || bufSamples == 0 || rateHz == 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)bufs * bufSamples * 1000u + rateHz - 1) / rateHz);
}

/* How long after the start the first sample written reaches the DAC, for the stop timer. */
static inline uint32_t notifyPopLeadMs(enum NotifyRing ring, uint32_t bufs, uint32_t bufSamples,
                                       uint32_t rateHz, bool sourceLoops) {
  const uint32_t trip = notifyRingTripMs(bufs, bufSamples, rateHz);
  switch (ring) {
    case NOTIFY_RING_FRESH:
      return trip;                            // exact, for either source
    case NOTIFY_RING_RESTARTED:
      return sourceLoops ? 0 : trip;          // [0, trip): early stops take the least, late the most
    default:
      return 0;
  }
}

/* The stop timer the caller arms: the sound's own stop (notifyPopStopMs) moved back by the lead.
 * Only zero padding (memory source) lies between the chirp and a late stop, so a stop past the
 * sound's end costs nothing but the pop's one-at-a-time window. */
static inline uint32_t notifyPopTimerMs(size_t pcmBytes, uint32_t rateHz, uint32_t marginMs,
                                        bool sourceLoops, uint32_t leadMs) {
  return notifyPopStopMs(pcmBytes, rateHz, marginMs, sourceLoops) + leadMs;
}

#endif // NOTIFY_TIMING_H
