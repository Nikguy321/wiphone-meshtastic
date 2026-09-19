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

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
