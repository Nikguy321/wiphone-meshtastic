/*
 * chunk_proto.h — the decision core of the chunked stop-and-wait upload.
 *
 * Why uploads are chunked at all (measured 2026-08-20, docs/upload-redesign-brief.md):
 * on a fast link a whole-file POST lets TCP run at line rate and the WiFi driver
 * floods internal heap with RX buffers below the application — 14 KB → 868 bytes in
 * seconds. One piece in flight, acknowledged before the next departs, bounds that
 * flood at the piece size BY CONSTRUCTION; no breaker or backpressure needed on the
 * happy path.
 *
 * Pure functions, no Arduino types — host-tested by tests/test_chunk.cpp. The server
 * half (app_gbc_xfer.cpp) supplies the SD facts and sends the verdict's reply:
 *
 *   POST /chunk?name=<fn>&off=<n>&crc=<hex8>[&last=1]   body: one multipart piece
 *     200 "ok <held>"    piece committed (or already held) — <held> is the
 *                        authoritative next offset; the client TRUSTS it
 *     409 "have <held>"  offset disagrees with the card — client resumes from <held>
 *     413 / 422 / 507    piece too big / CRC mismatch / SD write failed
 *   GET  /chunk?name=<fn>
 *     200 "<held>"       bytes of <fn> on the card right now (0 if absent)
 *
 * Idempotence rule: a piece is committed atomically (gathered whole in PSRAM,
 * CRC-checked, then ONE append), so the held size only ever grows by whole pieces —
 * a re-sent piece is either exactly at the held boundary (append) or entirely below
 * it (duplicate: ack without writing). Anything else is a resync.
 */

#ifndef CHUNK_PROTO_H
#define CHUNK_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum ChunkVerdict {
  CHUNK_APPEND,      // fresh piece at exactly the held boundary: commit it
  CHUNK_DUPLICATE,   // wholly below the boundary: a resend we already hold — ack, no write
  CHUNK_RESYNC,      // a gap or a straddle: tell the client what we hold, let it re-slice
  CHUNK_BAD          // malformed (empty piece)
};

static inline ChunkVerdict chunkDecide(size_t held, size_t off, size_t len) {
  if (len == 0) {
    return CHUNK_BAD;
  }
  if (off == held) {
    return CHUNK_APPEND;
  }
  if (off + len <= held && off + len > off) {   // whole piece behind us (and no overflow)
    return CHUNK_DUPLICATE;
  }
  return CHUNK_RESYNC;
}

/* The filename arrives from the network. Keep only the basename, refuse the
 * empty and the dot-names, cap the length. Returns false when nothing safe
 * survives — the caller answers 400, not a guess. */
static inline bool chunkSafeName(const char* in, char* out, size_t cap) {
  if (!in || cap < 2) {
    return false;
  }
  const char* base = in;
  for (const char* p = in; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  size_t n = strlen(base);
  if (n == 0 || n >= cap) {
    return false;
  }
  if (!strcmp(base, ".") || !strcmp(base, "..")) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if ((unsigned char)base[i] < 0x20) {        // control chars have no place in a FAT name
      return false;
    }
  }
  memcpy(out, base, n + 1);
  return true;
}

/* CRC32 (IEEE, the zlib/JS-standard one), bitwise — no table, so the firmware
 * and the host test compile the SAME code and the JS table version is checked
 * against it by vector ("123456789" → 0xCBF43926). ~1 ms per 4 KB piece at
 * 240 MHz, which vanishes next to the SD write it guards. */
static inline uint32_t chunkCrc32Init(void) {
  return 0xFFFFFFFFu;
}

static inline uint32_t chunkCrc32Update(uint32_t crc, const uint8_t* p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return crc;
}

static inline uint32_t chunkCrc32Final(uint32_t crc) {
  return crc ^ 0xFFFFFFFFu;
}

/* Exactly 8 hex digits — the CRC32 the page's JS sends. Anything looser
 * (empty string, trailing junk) is a client bug better caught loudly. */
static inline bool chunkParseHex32(const char* s, uint32_t* out) {
  if (!s || !out) {
    return false;
  }
  uint32_t v = 0;
  int i = 0;
  for (; s[i]; i++) {
    if (i >= 8) {
      return false;
    }
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') {
      d = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = (uint32_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      d = (uint32_t)(c - 'A' + 10);
    } else {
      return false;
    }
    v = (v << 4) | d;
  }
  if (i != 8) {
    return false;
  }
  *out = v;
  return true;
}

#endif // CHUNK_PROTO_H
