/*
 * book_hash.cpp — see book_hash.h for why these are implemented here rather than taken
 * from mbedtls.
 *
 * Straight FIPS 180-4 SHA-256 / SHA-1 and RFC 2104 HMAC. Nothing clever: the whole value of
 * this file is that it is boring and that tests/test_booksync.cpp proves it byte-for-byte
 * against COVEY's Python (which uses OpenSSL), so a subtle mistake here cannot reach the air.
 */
#include "book_hash.h"
#include <string.h>

// ---------------------------------------------------------------- SHA-256
static const uint32_t K256[64] = {
  0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
  0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
  0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
  0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
  0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
  0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
  0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
  0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256Block(uint32_t st[8], const uint8_t p[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + K256[i] + w[i];
    uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d;
  st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

void bsSha256Init(BsSha256* c) {
  c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
  c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
  c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
  c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
  c->bitCount = 0;
  c->bufLen = 0;
}

void bsSha256Update(BsSha256* c, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  c->bitCount += (uint64_t)len * 8;
  while (len) {
    size_t n = 64 - c->bufLen;
    if (n > len) {
      n = len;
    }
    memcpy(c->buf + c->bufLen, p, n);
    c->bufLen += n;
    p += n;
    len -= n;
    if (c->bufLen == 64) {
      sha256Block(c->state, c->buf);
      c->bufLen = 0;
    }
  }
}

void bsSha256Final(BsSha256* c, uint8_t out[32]) {
  uint64_t bits = c->bitCount;
  uint8_t pad = 0x80;
  bsSha256Update(c, &pad, 1);
  c->bitCount = bits;                     // padding is not part of the length
  uint8_t zero = 0;
  while (c->bufLen != 56) {
    bsSha256Update(c, &zero, 1);
    c->bitCount = bits;
  }
  uint8_t lenBe[8];
  for (int i = 0; i < 8; i++) {
    lenBe[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  bsSha256Update(c, lenBe, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4]     = (uint8_t)(c->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->state[i]);
  }
}

void bsSha256(const void* data, size_t len, uint8_t out[32]) {
  BsSha256 c;
  bsSha256Init(&c);
  bsSha256Update(&c, data, len);
  bsSha256Final(&c, out);
}

// ---------------------------------------------------------------- SHA-1
static void sha1Block(uint32_t st[5], const uint8_t p[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) {
    uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
    w[i] = (x << 1) | (x >> 31);
  }
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | ((~b) & d);        k = 0x5a827999u; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
    else             { f = b ^ c ^ d;                   k = 0xca62c1d6u; }
    uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
    e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void bsSha1Init(BsSha1* c) {
  c->state[0] = 0x67452301u; c->state[1] = 0xefcdab89u;
  c->state[2] = 0x98badcfeu; c->state[3] = 0x10325476u;
  c->state[4] = 0xc3d2e1f0u;
  c->bitCount = 0;
  c->bufLen = 0;
}

void bsSha1Update(BsSha1* c, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  c->bitCount += (uint64_t)len * 8;
  while (len) {
    size_t n = 64 - c->bufLen;
    if (n > len) {
      n = len;
    }
    memcpy(c->buf + c->bufLen, p, n);
    c->bufLen += n;
    p += n;
    len -= n;
    if (c->bufLen == 64) {
      sha1Block(c->state, c->buf);
      c->bufLen = 0;
    }
  }
}

void bsSha1Final(BsSha1* c, uint8_t out[20]) {
  uint64_t bits = c->bitCount;
  uint8_t pad = 0x80;
  bsSha1Update(c, &pad, 1);
  c->bitCount = bits;
  uint8_t zero = 0;
  while (c->bufLen != 56) {
    bsSha1Update(c, &zero, 1);
    c->bitCount = bits;
  }
  uint8_t lenBe[8];
  for (int i = 0; i < 8; i++) {
    lenBe[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  bsSha1Update(c, lenBe, 8);
  for (int i = 0; i < 5; i++) {
    out[i * 4]     = (uint8_t)(c->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->state[i]);
  }
}

// ---------------------------------------------------------------- HMAC-SHA256
void bsHmacSha256(const uint8_t* key, size_t keyLen,
                  const void* msg, size_t msgLen, uint8_t out[32]) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (keyLen > 64) {
    bsSha256(key, keyLen, k);            // keys longer than the block are hashed first
  } else {
    memcpy(k, key, keyLen);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36);
    opad[i] = (uint8_t)(k[i] ^ 0x5c);
  }
  uint8_t inner[32];
  BsSha256 c;
  bsSha256Init(&c);
  bsSha256Update(&c, ipad, 64);
  bsSha256Update(&c, msg, msgLen);
  bsSha256Final(&c, inner);

  bsSha256Init(&c);
  bsSha256Update(&c, opad, 64);
  bsSha256Update(&c, inner, 32);
  bsSha256Final(&c, out);
}

// ---------------------------------------------------------------- base32
static const char B32_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t bsBase32Encode(const uint8_t* src, size_t len, char* out, size_t cap) {
  size_t need = (len * 8 + 4) / 5;                 // ceil(bits/5); no padding written
  if (cap < need + 1) {
    return 0;
  }
  size_t oi = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    acc = (acc << 8) | src[i];
    bits += 8;
    while (bits >= 5) {
      out[oi++] = B32_ALPHA[(acc >> (bits - 5)) & 0x1F];
      bits -= 5;
    }
  }
  if (bits > 0) {
    out[oi++] = B32_ALPHA[(acc << (5 - bits)) & 0x1F];
  }
  out[oi] = '\0';
  return oi;
}

static int b32Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A';
  }
  if (ch >= '2' && ch <= '7') {
    return ch - '2' + 26;
  }
  return -1;                                        // includes '=', lower case, everything else
}

int bsBase32Decode(const char* src, size_t len, uint8_t* out, size_t cap) {
  if (len == 0) {
    return -1;
  }
  // Lengths that no real base32 body can have. Python's b32decode raises on these after
  // padding; refusing them here keeps the two implementations agreeing on "malformed".
  size_t rem = len % 8;
  if (rem == 1 || rem == 3 || rem == 6) {
    return -1;
  }
  size_t oi = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    int v = b32Value(src[i]);
    if (v < 0) {
      return -1;
    }
    acc = (acc << 5) | (uint32_t)v;
    bits += 5;
    if (bits >= 8) {
      if (oi >= cap) {
        return -1;
      }
      out[oi++] = (uint8_t)((acc >> (bits - 8)) & 0xFF);
      bits -= 8;
    }
  }
  return (int)oi;
}

// ---------------------------------------------------------------- UTF-8 truncation
size_t bsUtf8TruncBytes(char* dst, size_t cap, const char* src, size_t maxBytes) {
  size_t n = strlen(src);
  if (n > maxBytes) {
    n = maxBytes;
  }
  if (n >= cap) {
    n = cap - 1;
  }
  while (n > 0) {
    // Walk back to the start of the last character and check it arrived whole. Without
    // this, a byte cut through a multi-byte character leaves a fragment that Python's
    // "ignore" error handler would have dropped — and the two strings then hash differently.
    size_t start = n;
    while (start > 0 && ((uint8_t)src[start - 1] & 0xC0) == 0x80) {
      start--;
    }
    if (start == 0) {
      break;
    }
    uint8_t lead = (uint8_t)src[start - 1];
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0)      need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if (start - 1 + need <= n) {
      break;
    }
    n = start - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
  return n;
}

size_t bsUtf8TruncChars(char* dst, size_t cap, const char* src, size_t maxChars) {
  size_t chars = 0, i = 0;
  while (src[i] && chars < maxChars) {
    uint8_t c = (uint8_t)src[i];
    size_t step = 1;
    if ((c & 0xE0) == 0xC0)      step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    for (size_t k = 0; k < step && src[i]; k++) {
      i++;
    }
    chars++;
  }
  if (i >= cap) {
    i = cap - 1;
  }
  memcpy(dst, src, i);
  dst[i] = '\0';
  return i;
}

bool bsConstTimeEqual(const char* a, const char* b) {
  if (!a || !b) {
    return false;
  }
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < la; i++) {
    diff |= (uint8_t)(a[i] ^ b[i]);
  }
  return diff == 0;
}
