/* test_notify.cpp — the notification one-shot's stop timer, against the shipping sound.
 *
 * WHY THIS IS WORTH A SUITE: "sometimes the sound plays, sometimes it won't" and "sometimes
 * the motor just barely starts" were both the SAME fault — a timestamp taken at the top of a
 * main-loop pass, stamped onto the buzz and the pop after a mesh DB save had stalled that
 * pass by up to 1.5 s, so the next pass judged both finished a few ms after they began. The
 * stamps now come from millis() at the moment of the write; that part cannot be tested here.
 *
 * What CAN be tested is the arithmetic that replaced the literal 280 ms stop timer: it is
 * derived from the size of pop_pcm at the rate playPop() configures, and it has to stop EARLY
 * for the looping SPIFFS player (a late stop replays the attack, which is queued in the DMA
 * behind the tail) and LATE for the play-once memory source (an early stop cuts the tail).
 * The sound itself is included here so the claims the margin rests on — 90 ms of pop, then
 * silence to 320 ms, and an attack loud enough that replaying it would be heard — are pinned
 * to the bytes the phone ships, not to a comment.
 */
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "../WiPhone/notify_timing.h"
#include "../WiPhone/src/assets/pop_sound.h"

static int failures = 0;
static int checks = 0;

static void group(const char *name) {
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char *what) {
  checks++;
  if (!cond) {
    failures++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  } else {
    printf("  ok  %s\n", what);
  }
}

static int16_t sampleAt(size_t i) {
  // little-endian 16-bit signed, as the codec is fed
  return (int16_t)(pop_pcm[2 * i] | (pop_pcm[2 * i + 1] << 8));
}

/* ── A MODEL OF IDF 3.3's I2S TX RING, for the lead (integration review 3, A1) ────────────────
 * Just enough of driver/i2s.c to say WHEN each written sample reaches the DAC:
 *   - the DMA plays buffer `cur` a frame per tick and wraps round the ring;
 *   - at each buffer's end (the TX EOF interrupt) that buffer is pushed onto the free queue - and
 *     when the queue is full the oldest entry is dropped first (the ISR's underflow branch);
 *   - i2s_write(timeout 0) takes a buffer off the queue only when it holds one, and fills it
 *     from its start; nothing written goes anywhere else.
 *   - a FRESH ring's queue starts EMPTY (i2s_create_dma_queue() fills nothing); a RESTARTED one
 *     (i2s_start() on a stopped ring) keeps the queue the last session left, DMA back at buffer 0.
 * The writer is the LocalPcm memory source: pop_pcm's samples, then silence, written on each
 * loop pass (the notify pump is the pass at t = 0). The pop teardown at the stop timer is
 * ceasePlayback()'s i2s_zero_dma_buffer(): every buffer zeroed, so a sample not yet played is
 * never heard. Time is in frames (8 per ms at 8 kHz). */
struct RingModel {
  static const int kBufs = (int)NOTIFY_POP_RING_BUFS;
  static const int kLen = (int)NOTIFY_POP_RING_BUF_SAMPLES;
  int data[kBufs][kLen];      // the sample index a slot holds, -1 = silence
  int queue[kBufs];
  int qn = 0;
  int cur = 0, pos = 0;       // the DMA
  int wbuf = -1, wpos = 0;    // the writer's current buffer (IDF's curr_ptr / rw_pos)
  RingModel() {
    for (int b = 0; b < kBufs; b++) {
      for (int i = 0; i < kLen; i++) {
        data[b][i] = -1;
      }
    }
  }
  void push(int b) {
    if (qn == kBufs) {                       // full: the ISR drops the oldest first
      for (int i = 1; i < qn; i++) {
        queue[i - 1] = queue[i];
      }
      qn--;
    }
    queue[qn++] = b;
  }
  int pop() {
    const int b = queue[0];
    for (int i = 1; i < qn; i++) {
      queue[i - 1] = queue[i];
    }
    qn--;
    return b;
  }
  /* Write samples [next, total) and then silence, until the queue has nothing to give. */
  void write(int& next, int total) {
    for (;;) {
      if (wbuf < 0) {
        if (qn == 0) {
          return;                            // timeout 0: come back next pass
        }
        wbuf = pop();
        wpos = 0;
      }
      data[wbuf][wpos++] = next < total ? next++ : -1;
      if (wpos == kLen) {
        wbuf = -1;
      }
    }
  }
  /* One frame out of the DAC. Returns the sample index heard, or -1. */
  int tick() {
    const int s = data[cur][pos];
    if (++pos == kLen) {
      push(cur);
      cur = (cur + 1) % kBufs;
      pos = 0;
    }
    return s;
  }
};

/* Run one pop on the model. `queueInit` < 0: a FRESH ring (empty queue); otherwise a RESTARTED
 * one, its queue full and rotated by that many places. Returns how many of the first `want`
 * samples of pop_pcm reached the DAC before the teardown at `stopMs`. */
static int modelHeard(int queueInit, uint32_t stopMs, uint32_t passMs, int want) {
  RingModel m;
  if (queueInit >= 0) {
    for (int i = 0; i < RingModel::kBufs; i++) {
      m.push((i + queueInit) % RingModel::kBufs);
    }
  }
  const int total = (int)(sizeof(pop_pcm) / 2);
  const uint32_t fpm = NOTIFY_POP_RATE_HZ / 1000;        // frames per ms
  int next = 0;
  int heard = 0;
  for (uint32_t t = 0; t < stopMs * fpm; t++) {
    if (t % (passMs * fpm) == 0) {
      m.write(next, total);                  // the pump at t = 0, then every loop pass
    }
    const int s = m.tick();
    if (s >= 0 && s < want) {
      heard++;
    }
  }
  return heard;                              // the teardown zeroes what is left
}

int main() {
  group("notifyPcmMs: bytes at a rate -> playing time, rounded up");
  ok(notifyPcmMs(5120, 8000, 2) == 320, "5,120 bytes of 8 kHz 16-bit mono is 320 ms");
  ok(notifyPcmMs(1440, 8000, 2) == 90, "1,440 bytes (720 samples) is 90 ms");
  ok(notifyPcmMs(2, 8000, 2) == 1, "one sample rounds UP to 1 ms, never down to 0");
  ok(notifyPcmMs(1, 8000, 2) == 0, "a stray odd byte is no sample at all");
  ok(notifyPcmMs(5120, 0, 2) == 0 && notifyPcmMs(5120, 8000, 0) == 0,
     "a zero rate or sample size cannot divide by zero");
  ok(notifyPcmMs(5120, 16000, 2) == 160, "the same bytes at 16 kHz last half as long");

  group("notifyPopStopMs: the looping file player must stop EARLY");
  ok(notifyPopStopMs(5120, 8000, 40, true) == 280,
     "the shipping sound stops at 280 ms - the old literal, now derived");
  ok(notifyPopStopMs(5120, 8000, 40, true) < 320,
     "strictly before the wrap, so the second attack never sounds");
  ok(notifyPopStopMs(5120, 8000, 400, true) == 320,
     "a margin larger than the sound falls back to the sound's own length");

  group("notifyPopStopMs: the play-once memory source must stop LATE");
  ok(notifyPopStopMs(5120, 8000, 40, false) == 360,
     "the shipping sound stops at 360 ms - the whole array plus the margin");
  ok(notifyPopStopMs(5120, 8000, 40, false) > 320, "after the last sample has been queued");
  ok(notifyPopStopMs(5120, 8000, 40, false) < 640,
     "and well short of what a second full play would take");
  ok(notifyPopStopMs(160, 8000, 40, false) == 19,
     "a 10 ms sound with a 40 ms margin is capped at 19: no second play, ever");
  ok(notifyPopStopMs(0, 8000, 40, false) == 40 && notifyPopStopMs(0, 8000, 0, false) == 1,
     "an empty buffer still yields a non-zero timer, so the teardown always runs");

  group("the shipping pop_pcm: the facts the margin rests on");
  const size_t bytes = sizeof(pop_pcm);
  const size_t samples = bytes / 2;
  ok(bytes == 5120 && notifyPcmMs(bytes, NOTIFY_POP_RATE_HZ, NOTIFY_POP_BYTES_PER_SAMPLE) == 320,
     "5,120 bytes = 320 ms at the rate playPop() configures");
  size_t lastLoud = 0;
  for (size_t i = 0; i < samples; i++) {
    if (sampleAt(i) != 0) {
      lastLoud = i;
    }
  }
  const uint32_t audibleMs = (uint32_t)((lastLoud + 1) * 1000 / NOTIFY_POP_RATE_HZ);
  const uint32_t silenceMs = 320 - audibleMs;
  char line[128];
  snprintf(line, sizeof(line), "the sound is %u ms long and then silent for %u ms",
           (unsigned)audibleMs, (unsigned)silenceMs);
  ok(audibleMs == 90 && silenceMs == 230, line);
  ok(silenceMs >= NOTIFY_POP_MARGIN_MS,
     "so the file player's early stop cuts nothing but silence");
  int peakEarly = 0;
  for (size_t i = 0; i < NOTIFY_POP_MARGIN_MS * NOTIFY_POP_RATE_HZ / 1000; i++) {
    const int a = abs((int)sampleAt(i));
    if (a > peakEarly) {
      peakEarly = a;
    }
  }
  snprintf(line, sizeof(line), "the first %u ms peak at %d of 32767 - a replayed attack would be HEARD",
           (unsigned)NOTIFY_POP_MARGIN_MS, peakEarly);
  ok(peakEarly > 8000, line);

  group("the ring's lead: one trip round a fresh ring before the first written sample (A1)");
  const uint32_t trip = notifyRingTripMs(NOTIFY_POP_RING_BUFS, NOTIFY_POP_RING_BUF_SAMPLES,
                                         NOTIFY_POP_RATE_HZ);
  ok(trip == 512, "4 x 1024 mono at 8 kHz is 512 ms round");
  ok(notifyRingTripMs(4, 1023, 16000) == 256, "a stereo 1023 at 16 kHz rounds UP (255.75 -> 256)");
  ok(notifyRingTripMs(0, 1024, 8000) == 0 && notifyRingTripMs(4, 0, 8000) == 0 &&
     notifyRingTripMs(4, 1024, 0) == 0, "no ring (no driver) is no lead, and no divide by zero");
  ok(notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, false) == 512 &&
     notifyPopLeadMs(NOTIFY_RING_FRESH, 4, 1024, 8000, true) == 512,
     "FRESH: exactly one trip, for either source");
  ok(notifyPopLeadMs(NOTIFY_RING_RESTARTED, 4, 1024, 8000, false) == 512,
     "RESTARTED: the play-once source (stop LATE) takes the whole trip");
  ok(notifyPopLeadMs(NOTIFY_RING_RESTARTED, 4, 1024, 8000, true) == 0,
     "RESTARTED: the looping file (stop EARLY) takes none - its second attack is never replayed");
  ok(notifyPopLeadMs(NOTIFY_RING_RUNNING, 4, 1024, 8000, false) == 0 &&
     notifyPopLeadMs(NOTIFY_RING_RUNNING, 4, 1024, 8000, true) == 0,
     "RUNNING: the queue is full, no lead");

  group("the stop timer with the lead");
  const uint32_t flashFresh = notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ,
                                               NOTIFY_POP_MARGIN_MS, false, trip);
  ok(flashFresh == 872, "the shipping sound on a fresh ring stops at 872 ms (360 + 512)");
  ok(flashFresh > trip + 320, "after the array's last sample has reached the DAC (512 + 320)");
  ok(notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ, NOTIFY_POP_MARGIN_MS, false, 0) == 360,
     "with no lead it is the old 360, unchanged");
  const uint32_t fileFresh = notifyPopTimerMs(sizeof(pop_pcm), NOTIFY_POP_RATE_HZ,
                                              NOTIFY_POP_MARGIN_MS, true, trip);
  ok(fileFresh == 792 && fileFresh > trip + 90 && fileFresh < trip + 320,
     "the looping file on a fresh ring stops at 792: after the chirp (602), before the wrap (832)");

  group("the model: the old 360 ms stop erased the chirp on a fresh ring; the lead hears it");
  const int chirp = 720;                     // the 90 ms of pop, pinned above
  const int all = (int)(sizeof(pop_pcm) / 2);
  static const uint32_t passes[] = {5, 20, 50};
  bool oldSilent = true, newWhole = true, arrayWhole = true;
  for (size_t p = 0; p < sizeof(passes) / sizeof(passes[0]); p++) {
    oldSilent = oldSilent && modelHeard(-1, 360, passes[p], chirp) == 0;
    newWhole = newWhole && modelHeard(-1, flashFresh, passes[p], chirp) == chirp;
    arrayWhole = arrayWhole && modelHeard(-1, flashFresh, passes[p], all) == all;
  }
  ok(oldSilent, "FRESH ring, stop at 360 ms: 0 of 720 chirp samples heard, at 5/20/50 ms passes "
                "(the bug: the teardown zeroed them ~150 ms before they were due)");
  ok(newWhole, "FRESH ring, stop at 872 ms: all 720 heard, at every pass length");
  ok(arrayWhole, "...and the whole 2,560-sample array, not just the chirp");
  ok(modelHeard(-1, 511, 5, chirp) == 0 && modelHeard(-1, 513, 5, 1) == 1,
     "the first sample reaches the DAC at exactly one trip (512 ms), not before");
  int oldRestartMin = chirp, newRestartMin = chirp;
  for (int rot = 0; rot < RingModel::kBufs; rot++) {
    const int o = modelHeard(rot, 360, 20, chirp);
    const int n = modelHeard(rot, flashFresh, 20, chirp);
    oldRestartMin = o < oldRestartMin ? o : oldRestartMin;
    newRestartMin = n < newRestartMin ? n : newRestartMin;
  }
  snprintf(line, sizeof(line), "RESTARTED ring, every queue order: old stop hears as few as %d of "
           "720, the stop with the trip hears %d", oldRestartMin, newRestartMin);
  ok(oldRestartMin < chirp && newRestartMin == chirp, line);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
