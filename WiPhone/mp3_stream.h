/*
 * mp3_stream.h — file bytes in, decoded stereo frames out.
 *
 * The glue between an MP3 on the SD card and Audio's `playDec` buffer. Helix does the
 * decoding; this owns the awkward parts around it: skipping the ID3 tag, finding the
 * first real frame, and keeping the input buffer fed across reads that never line up
 * with frame boundaries.
 *
 * Arduino-free, so tests/test_mp3.cpp compiles it — and the REAL decoder, which is
 * portable C — with the Mac's compiler under ASan and decodes actual files. That is
 * worth the effort here: every failure mode in this layer is silent. A mis-skipped tag
 * or a sync word found inside album art does not crash, it produces noise or nothing.
 *
 * ── WHY THE OUTPUT IS A RING BUFFER AND NOT A FRAME ─────────────────────────────────
 *
 * Audio::loop() runs from the main loop, which also draws the screen, services WiFi,
 * writes to SD and runs the mesh. A decoder that hands over exactly one frame whenever
 * the main loop happens to ask would stutter every time something else took longer than
 * 26 ms — and screen redraws and SD reads both do.
 *
 * So decode runs AHEAD into a PSRAM ring and I2S drains it. The ring is the slack that
 * absorbs main-loop jitter, and it is why this does not need its own task on core 0:
 * a few hundred milliseconds of buffer is worth more than a scheduler here, and it does
 * not put a second thread anywhere near the I2S peripheral that SIP calls also use.
 */

#ifndef MP3_STREAM_H
#define MP3_STREAM_H

#include <stdint.h>
#include <stddef.h>

/* Biggest MPEG-1 Layer III frame is 1441 bytes (320 kbps, 44.1 kHz, with padding).
 * The input buffer holds several so a short read never starves a decode. */
#define MP3_INBUF_BYTES     4096

/* 1152 samples x 2 channels: one frame, and exactly what Audio::playDec is sized for. */
#define MP3_MAX_FRAME_SAMPLES 2304

struct Mp3Info {
  int sampleRate;
  int channels;
  int bitrate;
  int outputSamps;    // samples (not frames) in the last decode
};

/* Bytes to skip before MPEG audio starts, from the first 10 bytes of the file.
 *
 * ⚠ ID3v2 sizes are SYNCSAFE: 28 bits packed 7-per-byte, high bit always clear, so that
 * a tag can never contain a byte sequence resembling a frame sync. Reading it as a plain
 * big-endian integer gives a wrong (larger) answer on any tag over 128 bytes, which is
 * all of them — and the result is not an error, it is seeking past the start of the song.
 *
 * Returns 0 when there is no ID3v2 tag. Safe on short or hostile buffers. */
uint32_t mp3Id3v2Size(const uint8_t* buf, size_t len);

/* Offset of the first plausible frame sync at or after `from`, or -1.
 *
 * ⚠ A sync word is only eleven set bits, so it occurs constantly inside album art and
 * lyrics. This checks the rest of the header (a valid MPEG version, a layer, a bitrate
 * index that is neither `free` nor `bad`, and a real sample-rate index) before believing
 * it — which is what stops playback starting halfway through a JPEG. */
int mp3FindFrame(const uint8_t* buf, size_t len, size_t from);

class Mp3Stream {
public:
  Mp3Stream();
  ~Mp3Stream();

  /* Allocates the helix decoder (in PSRAM — see helix_memory.c). False if there is not
   * enough PSRAM, which is a refusal to play rather than a crash. */
  bool begin();
  void end();
  bool ready() const { return dec != 0; }

  /* Forget the current file's buffered bytes and decoder history. Call between tracks;
   * the decoder allocation is kept, because tearing 29 KB down and up between every
   * track is how PSRAM gets fragmented. */
  void reset();

  size_t space() const { return MP3_INBUF_BYTES - inLen; }
  size_t fill(const uint8_t* data, size_t len);   // returns bytes accepted

  /* Decode one frame into `out` (needs MP3_MAX_FRAME_SAMPLES shorts).
   * Returns samples written, 0 if more input is needed, -1 on a corrupt frame that was
   * skipped. A corrupt frame is NOT fatal: MP3 is designed to resynchronise, and a
   * damaged download should cost a crackle, not the rest of the album. */
  int decode(int16_t* out, Mp3Info* info);

  /* True once a frame has decoded, so the caller knows the real rate and channel count.
   * ⚠ Do not configure I2S from the first frame HEADER — configure it from the first
   * successful DECODE. A header can be read out of a false sync; a decode cannot. */
  bool haveFormat() const { return gotFormat; }
  const Mp3Info& format() const { return fmt; }

private:
  void*   dec;                       // HMP3Decoder
  uint8_t inBuf[MP3_INBUF_BYTES];
  size_t  inLen;
  bool    gotFormat;
  Mp3Info fmt;

  void discard(size_t n);
};

#endif // MP3_STREAM_H
