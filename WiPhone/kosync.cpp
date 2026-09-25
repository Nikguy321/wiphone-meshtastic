/*
 * kosync.cpp — see kosync.h. No Arduino headers: tests/test_kosync.cpp builds this file.
 */
#include "kosync.h"
#include "book_hash.h"
#include "booksync.h"
#include "booksync_inbox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- small helpers
static char ksLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ksEqNoCase(const char* a, size_t alen, const char* b) {
  const size_t blen = strlen(b);
  if (alen != blen) {
    return false;
  }
  for (size_t i = 0; i < alen; i++) {
    if (ksLower(a[i]) != ksLower(b[i])) {
      return false;
    }
  }
  return true;
}

static void ksCopy(char* out, size_t cap, const char* in) {
  if (cap == 0) {
    return;
  }
  size_t n = in ? strlen(in) : 0;
  if (n > cap - 1) {
    n = cap - 1;
  }
  if (n) {
    memcpy(out, in, n);
  }
  out[n] = '\0';
}

// ---------------------------------------------------------------- numbers
size_t kosyncFormatPct(double p, char* out, size_t cap) {
  if (!(p >= 0.0)) {              // negative, and NaN
    p = 0.0;
  }
  if (p > 1.0) {
    p = 1.0;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%.6f", p);
  size_t n = strlen(buf);
  if (strchr(buf, '.')) {
    while (n > 0 && buf[n - 1] == '0') {
      buf[--n] = '\0';
    }
    if (n > 0 && buf[n - 1] == '.') {
      buf[--n] = '\0';
    }
  }
  if (n == 0 || !strcmp(buf, "-0")) {
    strcpy(buf, "0");
  }
  ksCopy(out, cap, buf);
  return strlen(out);
}

// ---------------------------------------------------------------- JSON
static const char* jsWs(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
    p++;
  }
  return p;
}

static int jsHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static size_t jsPutUtf8(uint32_t cp, char* o) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static bool jsU4(const char* p, const char* e, uint32_t* v) {
  if (e - p < 4) {
    return false;
  }
  uint32_t x = 0;
  for (int i = 0; i < 4; i++) {
    const int h = jsHex(p[i]);
    if (h < 0) {
      return false;
    }
    x = (x << 4) | (uint32_t)h;
  }
  *v = x;
  return true;
}

/* A string starting at the opening quote. Decoded into `out` (cut at cap, never overrun);
 * returns the character after the closing quote, or NULL when it never closes. */
static const char* jsString(const char* p, const char* e, char* out, size_t cap) {
  size_t n = 0;
  if (p >= e || *p != '"') {
    return NULL;
  }
  p++;
  while (p < e && *p != '"') {
    char enc[4];
    size_t el = 0;
    if (*p == '\\') {
      if (p + 1 >= e) {
        return NULL;
      }
      const char c = p[1];
      p += 2;
      switch (c) {
        case '"': enc[0] = '"'; el = 1; break;
        case '\\': enc[0] = '\\'; el = 1; break;
        case '/': enc[0] = '/'; el = 1; break;
        case 'b': enc[0] = '\b'; el = 1; break;
        case 'f': enc[0] = '\f'; el = 1; break;
        case 'n': enc[0] = '\n'; el = 1; break;
        case 'r': enc[0] = '\r'; el = 1; break;
        case 't': enc[0] = '\t'; el = 1; break;
        case 'u': {
          uint32_t cp = 0;
          if (!jsU4(p, e, &cp)) {
            return NULL;
          }
          p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF) {              // a surrogate pair, if one follows
            uint32_t lo = 0;
            if (e - p >= 6 && p[0] == '\\' && p[1] == 'u' && jsU4(p + 2, e, &lo) &&
                lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p += 6;
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;                                   // a lone low surrogate
          }
          el = jsPutUtf8(cp, enc);
          break;
        }
        default:
          return NULL;                                     // not JSON
      }
    } else {
      enc[0] = *p++;
      el = 1;
    }
    if (out && n + el < cap) {
      memcpy(out + n, enc, el);
      n += el;
    }
  }
  if (p >= e) {
    return NULL;
  }
  if (out && cap) {
    out[n] = '\0';
  }
  return p + 1;
}

// A number token: returns the character after it, or NULL if there is none here.
static const char* jsNumber(const char* p, const char* e, double* v) {
  char tok[40];
  size_t n = 0;
  while (p < e && n < sizeof(tok) - 1 &&
         ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
    tok[n++] = *p++;
  }
  if (n == 0) {
    return NULL;
  }
  tok[n] = '\0';
  char* end = NULL;
  const double d = strtod(tok, &end);
  if (!end || *end != '\0') {
    return NULL;
  }
  *v = d;
  return p;
}

static const char* jsSkip(const char* p, const char* e, int depth) {
  p = jsWs(p, e);
  if (p >= e || depth > 16) {
    return NULL;
  }
  if (*p == '"') {
    return jsString(p, e, NULL, 0);
  }
  if (*p == '{' || *p == '[') {
    const char close = (*p == '{') ? '}' : ']';
    const bool obj = (*p == '{');
    p = jsWs(p + 1, e);
    if (p < e && *p == close) {
      return p + 1;
    }
    for (;;) {
      if (obj) {
        p = jsString(jsWs(p, e), e, NULL, 0);
        if (!p) return NULL;
        p = jsWs(p, e);
        if (p >= e || *p != ':') return NULL;
        p++;
      }
      p = jsSkip(p, e, depth + 1);
      if (!p) return NULL;
      p = jsWs(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == close) return p + 1;
      return NULL;
    }
  }
  if (e - p >= 4 && (!strncmp(p, "true", 4) || !strncmp(p, "null", 4))) {
    return p + 4;
  }
  if (e - p >= 5 && !strncmp(p, "false", 5)) {
    return p + 5;
  }
  double d;
  return jsNumber(p, e, &d);
}

bool kosyncParseProgress(const char* body, size_t len, KosyncProgress* out) {
  memset(out, 0, sizeof(*out));
  if (!body) {
    return false;
  }
  const char* e = body + len;
  const char* p = jsWs(body, e);
  if (p >= e || *p != '{') {
    return false;
  }
  p = jsWs(p + 1, e);
  if (p < e && *p == '}') {
    return true;                        // `{}`: nobody has synced this document
  }
  for (;;) {
    char key[24];
    p = jsString(jsWs(p, e), e, key, sizeof(key));
    if (!p) {
      return false;
    }
    p = jsWs(p, e);
    if (p >= e || *p != ':') {
      return false;
    }
    p = jsWs(p + 1, e);
    if (p >= e) {
      return false;
    }
    const char* next = NULL;
    if (!strcmp(key, "percentage") || !strcmp(key, "timestamp")) {
      double d = 0.0;
      bool have = false;
      if (*p == '"') {                  // a server that quotes its numbers: still a number
        char s[40];
        next = jsString(p, e, s, sizeof(s));
        char* end = NULL;
        d = strtod(s, &end);
        have = next && end && end != s && *end == '\0';
      } else if (*p == 'n') {
        next = jsSkip(p, e, 0);         // null: absent
      } else {
        next = jsNumber(p, e, &d);
        have = next != NULL;
      }
      if (have && !strcmp(key, "percentage") && isfinite(d)) {
        out->hasPct = true;
        out->pct = d;
      } else if (have && isfinite(d)) {
        out->hasTimestamp = true;
        out->timestamp = (int64_t)d;
      }
    } else if (!strcmp(key, "device") || !strcmp(key, "device_id") || !strcmp(key, "document")) {
      char* dst = !strcmp(key, "device") ? out->device
                : !strcmp(key, "device_id") ? out->deviceId : out->document;
      const size_t cap = !strcmp(key, "document") ? sizeof(out->document) : sizeof(out->device);
      if (*p == '"') {
        next = jsString(p, e, dst, cap);
      } else {
        next = jsSkip(p, e, 0);         // a number or null where a name should be: ignore it
      }
    } else {
      next = jsSkip(p, e, 0);           // progress, metadata, anything newer
    }
    if (!next) {
      return false;
    }
    p = jsWs(next, e);
    if (p >= e) {
      return false;
    }
    if (*p == ',') {
      p++;
      continue;
    }
    return *p == '}';
  }
}

size_t kosyncJsonEscape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  if (!cap) {
    return 0;
  }
  for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p; p++) {
    char tmp[8];
    size_t tl;
    if (*p == '"' || *p == '\\') {
      tmp[0] = '\\'; tmp[1] = (char)*p; tl = 2;
    } else if (*p < 0x20) {
      tl = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)*p);
    } else {
      tmp[0] = (char)*p; tl = 1;
    }
    if (n + tl >= cap) {
      break;
    }
    memcpy(out + n, tmp, tl);
    n += tl;
  }
  out[n] = '\0';
  return n;
}

// ---------------------------------------------------------------- HTTP
bool kosyncHeader(const char* hdrs, const char* name, char* out, size_t cap) {
  if (cap) {
    out[0] = '\0';
  }
  if (!hdrs || !name) {
    return false;
  }
  const char* line = strchr(hdrs, '\n');           // skip the request/status line
  while (line) {
    line++;
    const char* eol = line;
    while (*eol && *eol != '\n') {
      eol++;
    }
    const char* end = eol;
    if (end > line && end[-1] == '\r') {
      end--;
    }
    if (end == line) {
      return false;                                // the blank line: end of headers
    }
    const char* colon = (const char*)memchr(line, ':', (size_t)(end - line));
    if (colon && ksEqNoCase(line, (size_t)(colon - line), name)) {
      const char* v = colon + 1;
      while (v < end && (*v == ' ' || *v == '\t')) {
        v++;
      }
      const char* ve = end;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) {
        ve--;
      }
      size_t n = (size_t)(ve - v);
      if (cap) {
        if (n > cap - 1) {
          n = cap - 1;
        }
        memcpy(out, v, n);
        out[n] = '\0';
      }
      return true;
    }
    line = *eol ? eol : NULL;
  }
  return false;
}

bool kosyncAuthOk(const char* hdrs, const char* user, const char* keyHex) {
  if (!user || !user[0] || !keyHex || !keyHex[0]) {
    return false;                                  // nothing configured authorises nobody
  }
  char u[KOSYNC_USER_MAX + 8], k[KOSYNC_KEY_CHARS + 8];
  if (!kosyncHeader(hdrs, "x-auth-user", u, sizeof(u)) ||
      !kosyncHeader(hdrs, "x-auth-key", k, sizeof(k))) {
    return false;
  }
  const bool userOk = bsConstTimeEqual(u, user);
  const bool keyOk = bsConstTimeEqual(k, keyHex);  // both always evaluated
  return userOk && keyOk;
}

int kosyncStatusCode(const char* line) {
  if (!line || strncmp(line, "HTTP/1.", 7) != 0) {
    return -1;
  }
  const char* sp = strchr(line, ' ');
  if (!sp) {
    return -1;
  }
  const int c = atoi(sp + 1);
  return (c >= 100 && c <= 599) ? c : -1;
}

long kosyncDechunk(char* buf, size_t len) {
  size_t r = 0, w = 0;
  for (;;) {
    size_t size = 0;
    int digits = 0;
    while (r < len) {
      const char c = buf[r];
      int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
            : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (h < 0) {
        break;
      }
      if (size > (len >> 4) + 1) {
        return -1;                     // bigger than anything this buffer could hold
      }
      size = size * 16 + (size_t)h;
      digits++;
      r++;
    }
    if (!digits) {
      return -1;
    }
    while (r < len && buf[r] != '\n') {
      r++;                             // an extension (";name=value") and the CR
    }
    if (r >= len) {
      return -1;
    }
    r++;                               // the LF
    if (size == 0) {
      return (long)w;                  // the last chunk; any trailers are dropped
    }
    if (size > len - r) {
      return -1;                       // a chunk that runs past what arrived
    }
    memmove(buf + w, buf + r, size);   // w <= r always: safe in place
    w += size;
    r += size;
    if (r < len && buf[r] == '\r') {
      r++;
    }
    if (r >= len || buf[r] != '\n') {
      return -1;
    }
    r++;
  }
}

static int ksHost(char* out, size_t cap, const char* host, uint16_t port) {
  return port == 80 ? snprintf(out, cap, "%s", host) : snprintf(out, cap, "%s:%u", host, (unsigned)port);
}

size_t kosyncBuildGet(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* doc) {
  char h[KOSYNC_HOST_MAX + 8];
  ksHost(h, sizeof(h), host, port);
  const int n = snprintf(out, cap,
                         "GET /syncs/progress/%s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Accept: " KOSYNC_ACCEPT "\r\n"
                         "x-auth-user: %s\r\n"
                         "x-auth-key: %s\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         doc, h, user, key);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t kosyncBuildPut(char* out, size_t cap, const char* host, uint16_t port,
                      const char* user, const char* key, const char* body) {
  char h[KOSYNC_HOST_MAX + 8];
  ksHost(h, sizeof(h), host, port);
  const int n = snprintf(out, cap,
                         "PUT /syncs/progress HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Accept: " KOSYNC_ACCEPT "\r\n"
                         "Content-Type: application/json\r\n"
                         "x-auth-user: %s\r\n"
                         "x-auth-key: %s\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "%s",
                         h, user, key, (unsigned)strlen(body), body);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t kosyncBuildPutBody(char* out, size_t cap, const char* doc, double pct,
                          const char* device, const char* deviceId) {
  char p[16], d[KOSYNC_DEV_MAX * 2], di[KOSYNC_DEV_MAX * 2], dc[KOSYNC_DOC_MAX * 2];
  kosyncFormatPct(pct, p, sizeof(p));
  kosyncJsonEscape(device, d, sizeof(d));
  kosyncJsonEscape(deviceId, di, sizeof(di));
  kosyncJsonEscape(doc, dc, sizeof(dc));
  const int n = snprintf(out, cap,
                         "{\"document\":\"%s\",\"progress\":\"\",\"percentage\":%s,"
                         "\"device\":\"%s\",\"device_id\":\"%s\"}",
                         dc, p, d, di);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// ---------------------------------------------------------------- the server
bool kosyncIsPath(const char* path) {
  return path && (!strncmp(path, "/users/", 7) || !strncmp(path, "/syncs/", 7) ||
                  !strcmp(path, "/healthcheck"));
}

KosyncRoute kosyncRoute(const char* method, const char* path, char* doc, size_t cap) {
  if (cap) {
    doc[0] = '\0';
  }
  if (!method || !path) {
    return KOSYNC_ROUTE_NONE;
  }
  if (!strcmp(path, "/healthcheck") && !strcmp(method, "GET")) {
    return KOSYNC_ROUTE_HEALTH;
  }
  if (!strcmp(path, "/users/auth") && !strcmp(method, "GET")) {
    return KOSYNC_ROUTE_AUTH;
  }
  if (!strcmp(path, "/users/create")) {
    return KOSYNC_ROUTE_CREATE;
  }
  if (!strcmp(path, "/syncs/progress") && !strcmp(method, "PUT")) {
    return KOSYNC_ROUTE_PUT_PROGRESS;
  }
  static const char PFX[] = "/syncs/progress/";
  if (!strncmp(path, PFX, sizeof(PFX) - 1) && !strcmp(method, "GET")) {
    const char* d = path + sizeof(PFX) - 1;
    if (!*d || strchr(d, '/') || strlen(d) >= cap) {
      return KOSYNC_ROUTE_NONE;
    }
    ksCopy(doc, cap, d);
    return KOSYNC_ROUTE_GET_PROGRESS;
  }
  return KOSYNC_ROUTE_NONE;
}

bool kosyncIsOwnRecord(const char* theirDevice, const char* theirDeviceId,
                       const char* myDevice, const char* myDeviceId) {
  if (theirDevice && theirDevice[0] && myDevice && !strcmp(theirDevice, myDevice)) {
    return true;                        // our own record, coming back from a server
  }
  return theirDeviceId && theirDeviceId[0] && myDeviceId && !strcmp(theirDeviceId, myDeviceId);
}

bool kosyncWorthParking(double theirs, const char* theirDevice, const char* theirDeviceId,
                        double mine, const char* myDevice, const char* myDeviceId) {
  if (kosyncIsOwnRecord(theirDevice, theirDeviceId, myDevice, myDeviceId)) {
    return false;
  }
  // The same slack every device uses: CrossPoint calls this "synchronized".
  return fabs(theirs - mine) > KOSYNC_EPSILON + 1e-9;
}

bool kosyncPullIsNews(double theirs, bool theirHasTs, int64_t theirTs,
                      double mine, bool mineOk, uint32_t myTurnedAt) {
  if (!mineOk) {
    return true;                        // we cannot say where we are: let the person decide
  }
  if (theirHasTs && theirTs > 0 && myTurnedAt > 0) {
    return theirTs > (int64_t)myTurnedAt;
  }
  return theirs > mine + KOSYNC_EPSILON + 1e-9;
}

static void ksReply(char* reply, size_t cap, const char* s) {
  ksCopy(reply, cap, s);
}

void kosyncServe(const char* method, const char* path, const char* hdrs,
                 const char* body, size_t bodyLen,
                 const char* user, const char* key, const KosyncServed* book,
                 uint32_t nowUnix, KosyncServeOut* out, char* reply, size_t replyCap) {
  memset(out, 0, sizeof(*out));
  char doc[KOSYNC_DOC_MAX];
  const KosyncRoute r = kosyncRoute(method, path, doc, sizeof(doc));
  switch (r) {
    case KOSYNC_ROUTE_HEALTH:
      out->code = 200;
      ksReply(reply, replyCap, "{\"state\":\"OK\"}");
      return;
    case KOSYNC_ROUTE_CREATE:
      /* One account, from the card. Registering over the air would mean anyone who joins the
       * open hotspot could make themselves a user. */
      out->code = 402;
      ksReply(reply, replyCap, "{\"message\":\"User registration is disabled.\",\"code\":2005}");
      return;
    case KOSYNC_ROUTE_NONE:
      out->code = 404;
      ksReply(reply, replyCap, "{\"message\":\"Not found\"}");
      return;
    default:
      break;
  }
  if (!kosyncAuthOk(hdrs, user, key)) {
    out->code = 401;
    ksReply(reply, replyCap, "{\"message\":\"Unauthorized\",\"code\":2001}");
    return;
  }
  if (r == KOSYNC_ROUTE_AUTH) {
    out->code = 200;
    ksReply(reply, replyCap, "{\"authorized\":\"OK\"}");
    return;
  }
  const bool haveBook = book && book->partial && book->byName;
  if (r == KOSYNC_ROUTE_GET_PROGRESS) {
    out->code = 200;
    if (!haveBook || (strcmp(doc, book->partial) != 0 && strcmp(doc, book->byName) != 0)) {
      ksReply(reply, replyCap, "{}");   // 🛑 NOT a 404: see kosync.h
      return;
    }
    out->pickedUp = true;               // our book, whatever we can say about it
    if (!book->pctOk) {
      /* The phone is somewhere it cannot express as a KOSync percentage (a chapter CrossPoint
       * does not list). `{}` is the honest answer — "nothing to offer" — where a 0 or the
       * place from before the reader moved would send the peer somewhere wrong. */
      ksReply(reply, replyCap, "{}");
      return;
    }
    char p[16], d[KOSYNC_DEV_MAX * 2], di[KOSYNC_DEV_MAX * 2];
    kosyncFormatPct(book->pct, p, sizeof(p));
    kosyncJsonEscape(book->device, d, sizeof(d));
    kosyncJsonEscape(book->deviceId, di, sizeof(di));
    // `doc` equals one of our two hex ids here, so it needs no escaping.
    snprintf(reply, replyCap,
             "{\"document\":\"%s\",\"percentage\":%s,\"progress\":\"\",\"device\":\"%s\","
             "\"device_id\":\"%s\",\"timestamp\":%lu}",
             doc, p, d, di, (unsigned long)book->turnedAt);
    return;
  }
  // PUT /syncs/progress
  KosyncProgress in;
  if (!kosyncParseProgress(body, bodyLen, &in) || !in.document[0] || !in.hasPct) {
    out->code = 400;
    ksReply(reply, replyCap, "{\"message\":\"Invalid request\",\"code\":2003}");
    return;
  }
  char dc[KOSYNC_DOC_MAX * 2];
  kosyncJsonEscape(in.document, dc, sizeof(dc));
  out->code = 200;
  snprintf(reply, replyCap, "{\"document\":\"%s\",\"timestamp\":%lu}", dc, (unsigned long)nowUnix);
  if (!haveBook || (strcmp(in.document, book->partial) != 0 && strcmp(in.document, book->byName) != 0)) {
    out->otherDoc = true;               // answered, and dropped: this window is one book
    return;
  }
  out->pickedUp = true;
  out->gotPut = true;
  out->putPct = in.pct < 0.0 ? 0.0 : (in.pct > 1.0 ? 1.0 : in.pct);
  ksCopy(out->putDevice, sizeof(out->putDevice), in.device);
  ksCopy(out->putDeviceId, sizeof(out->putDeviceId), in.deviceId);
  // With no place of our own to compare against, any other device's place is worth a card.
  out->park = book->pctOk
              ? kosyncWorthParking(out->putPct, in.device, in.deviceId,
                                   book->pct, book->device, book->deviceId)
              : !kosyncIsOwnRecord(in.device, in.deviceId, book->device, book->deviceId);
}

// ---------------------------------------------------------------- the window's clock
void kosyncClockOpen(KosyncWindowClock* c, uint32_t now, uint32_t durationMs) {
  c->open = true;
  c->openedMs = now;
  c->durationMs = durationMs;
  c->picked = false;
  c->pickedMs = 0;
  c->graceMs = 0;
}

void kosyncClockPicked(KosyncWindowClock* c, uint32_t now, uint32_t graceMs) {
  if (!c->open) {
    return;
  }
  c->picked = true;
  c->pickedMs = now;
  c->graceMs = graceMs;
}

int kosyncClockDue(const KosyncWindowClock* c, uint32_t now) {
  if (!c->open) {
    return KOSYNC_CLOCK_CLOSED;
  }
  // Unsigned differences: correct across the 49-day millis() wrap.
  if ((uint32_t)(now - c->openedMs) >= c->durationMs) {
    return KOSYNC_CLOCK_DEADLINE;
  }
  if (c->picked && (uint32_t)(now - c->pickedMs) >= c->graceMs) {
    return KOSYNC_CLOCK_PICKED;
  }
  return KOSYNC_CLOCK_RUNNING;
}

uint32_t kosyncClockRemainingMs(const KosyncWindowClock* c, uint32_t now) {
  if (kosyncClockDue(c, now) != KOSYNC_CLOCK_RUNNING) {
    return 0;
  }
  uint32_t left = c->durationMs - (uint32_t)(now - c->openedMs);
  if (c->picked) {
    const uint32_t g = c->graceMs - (uint32_t)(now - c->pickedMs);
    if (g < left) {
      left = g;
    }
  }
  return left;
}

// ---------------------------------------------------------------- config
static bool ksBool(const char* v, bool* out) {
  static const char* ON[] = { "on", "yes", "true", "1" };
  static const char* OFF[] = { "off", "no", "false", "0" };
  for (int i = 0; i < 4; i++) {
    if (ksEqNoCase(v, strlen(v), ON[i])) { *out = true; return true; }
    if (ksEqNoCase(v, strlen(v), OFF[i])) { *out = false; return true; }
  }
  return false;
}

static bool ksIsHex32(const char* v) {
  if (strlen(v) != 32) {
    return false;
  }
  for (int i = 0; i < 32; i++) {
    const char c = v[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool kosyncHotspotPassValid(const char* v, const char** why) {
  const size_t n = v ? strlen(v) : 0;
  if (n < 8 || n > 63) {
    /* softAP() itself refuses 1-7 (and would then bring NO hotspot up at all), and its
     * config field holds 63; a 64-hex raw PSK is not accepted here either. */
    if (why) *why = "hotspot_pass ignored: 8-63 characters";
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c < 0x20 || c > 0x7E) {
      if (why) *why = "hotspot_pass ignored: plain ASCII only";
      return false;
    }
  }
  if (why) *why = "";
  return true;
}

bool kosyncParseConfig(const char* text, size_t len, KosyncConfig* out) {
  memset(out, 0, sizeof(*out));
  out->homePort = 80;
  bool havePw = false;
  const char* p = text;
  const char* e = text ? text + len : text;
  char line[160];
  while (p && p < e) {
    const char* eol = p;
    while (eol < e && *eol != '\n') {
      eol++;
    }
    size_t n = (size_t)(eol - p);
    if (n > sizeof(line) - 1) {
      n = sizeof(line) - 1;           // an absurd line: cut, and it will not parse as a key
    }
    memcpy(line, p, n);
    line[n] = '\0';
    p = eol < e ? eol + 1 : e;
    // Trim both ends (CR included: the file may come from Windows).
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    char* t = s + strlen(s);
    while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) *--t = '\0';
    if (!*s || *s == '#') {
      continue;
    }
    char* eq = strchr(s, '=');
    if (!eq) {
      continue;
    }
    *eq = '\0';
    char* k = s;
    char* v = eq + 1;
    char* ke = eq;
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
    while (*v == ' ' || *v == '\t') v++;
    for (char* q = k; *q; q++) *q = ksLower(*q);

    if (!strcmp(k, "user")) {
      ksCopy(out->user, sizeof(out->user), v);
    } else if (!strcmp(k, "password")) {
      if (*v) {
        bsMd5Hex(v, strlen(v), out->key);
        out->passwordShort = strlen(v) < 12;
        havePw = true;
      }
      memset(line, 0, sizeof(line));  // the plaintext does not outlive this line
    } else if (!strcmp(k, "key")) {
      if (ksIsHex32(v)) {
        for (int i = 0; i < 32; i++) out->key[i] = ksLower(v[i]);
        out->key[32] = '\0';
        havePw = true;
      } else {
        ksCopy(out->problem, sizeof(out->problem), "key= must be 32 hex digits (MD5 of the password)");
      }
    } else if (!strcmp(k, "home")) {
      if (!strncmp(v, "https://", 8)) {
        ksCopy(out->problem, sizeof(out->problem), "home= must be http:// (no TLS here)");
        continue;
      }
      if (!strncmp(v, "http://", 7)) {
        v += 7;
      }
      char h[KOSYNC_HOST_MAX + 8];
      ksCopy(h, sizeof(h), v);
      size_t hl = strlen(h);
      while (hl && h[hl - 1] == '/') h[--hl] = '\0';
      char* colon = strrchr(h, ':');
      if (colon) {
        *colon = '\0';
        const long port = strtol(colon + 1, NULL, 10);
        if (port > 0 && port < 65536) {
          out->homePort = (uint16_t)port;
        } else {
          ksCopy(out->problem, sizeof(out->problem), "home= has a bad port");
        }
      }
      if (strchr(h, '/')) {
        ksCopy(out->problem, sizeof(out->problem), "home= takes a host[:port], not a path");
        continue;
      }
      ksCopy(out->home, sizeof(out->home), h);
    } else if (!strcmp(k, "device")) {
      ksCopy(out->device, sizeof(out->device), v);
    } else if (!strcmp(k, "auto")) {
      ksBool(v, &out->autoOnClose);
    } else if (!strcmp(k, "open_window")) {
      ksBool(v, &out->openWindow);
    } else if (!strcmp(k, "hotspot_pass")) {
      /* Leading/trailing spaces are trimmed with the line (a Windows CR too); the password
       * is what is between them. Invalid -> OPEN, and hotspotNote says why — a hotspot that
       * is silently open when its owner wrote a password is the one failure worth avoiding. */
      const char* why = "";
      if (kosyncHotspotPassValid(v, &why)) {
        ksCopy(out->hotspotPass, sizeof(out->hotspotPass), v);
        out->hotspotNote[0] = '\0';
      } else {
        out->hotspotPass[0] = '\0';
        ksCopy(out->hotspotNote, sizeof(out->hotspotNote), why);
      }
      memset(line, 0, sizeof(line));  // no stray copy of it on the stack
    }
  }
  if (!out->user[0] || !havePw) {
    if (!out->problem[0]) {
      ksCopy(out->problem, sizeof(out->problem), !out->user[0] ? "no user= line" : "no password= line");
    }
    out->ok = false;
    return false;
  }
  out->ok = true;
  if (out->passwordShort && !out->problem[0]) {
    ksCopy(out->problem, sizeof(out->problem), "password is under 12 characters");
  }
  return true;
}

void kosyncDeviceId(const uint8_t mac[6], char out[KOSYNC_KEY_CHARS]) {
  char s[24];
  snprintf(s, sizeof(s), "WiPhone-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  bsMd5Hex(s, strlen(s), out);
}

// ---------------------------------------------------------------- into the booksync inbox
double kosyncToReaderFraction(int readIdx, double readWithin, int nRead) {
  const int n = nRead > 0 ? nRead : 1;
  double w = readWithin;
  if (!(w >= 0.0)) {
    w = 0.0;
  }
  if (w > 1.0) {
    w = 1.0;
  }
  double f = ((double)readIdx + w) / (double)n + 0.5 / 65535.0;   // see kosync.h
  if (f < 0.0) {
    f = 0.0;
  }
  if (f > 1.0) {
    f = 1.0;
  }
  return f;
}

bool kosyncParkText(const char* const* ids, int nIds, int readIdx, double readWithin, int nRead,
                    uint32_t turnedAt, const char* device, const uint8_t key[32],
                    char* out, size_t cap) {
  if (!ids || nIds <= 0 || readIdx < 0 || readIdx >= nRead) {
    return false;
  }
  BookSyncRecord r;
  /* offset 0: the peer's byte offset means nothing in OUR extraction, and applyPending() goes
   * by the fraction anyway (epubLocate), exactly as it does for a COVEY packet. */
  bookSyncMakeRecord(&r, ids, nIds, (uint32_t)readIdx, 0,
                     kosyncToReaderFraction(readIdx, readWithin, nRead), turnedAt,
                     (device && device[0]) ? device : "KOSync", NULL);
  return bookSyncPackMesh(&r, key, out, cap);
}

// ---------------------------------------------------------------- the park ledger
static int ledgerFind(const KosyncParkLedger* l, const char* book) {
  for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
    if (l->book[i][0] && !strcmp(l->book[i], book)) {
      return i;
    }
  }
  return -1;
}

bool kosyncParkInto(KosyncParkLedger* l, const char* book, const char* text, double peerPct,
                    uint32_t rxUnix) {
  if (!l || !book || !book[0] || !text) {
    return false;
  }
  int slot = ledgerFind(l, book);
  if (slot >= 0 && l->id[slot]) {
    bookSyncInboxRemoveId(l->id[slot]);   // the offer it replaces; already-gone is fine
    l->id[slot] = 0;
  }
  if (!bookSyncInboxPush(text, 0, rxUnix)) {
    return false;
  }
  uint32_t id = 0;
  for (int i = 0; i < bookSyncInboxCount(); i++) {
    const BookSyncInboxItem* it = bookSyncInboxGet(i);
    if (it && !strcmp(it->text, text)) {
      id = it->id;
      break;
    }
  }
  if (slot < 0) {
    slot = l->next;
    l->next = (l->next + 1) % KOSYNC_LEDGER_MAX;
  }
  ksCopy(l->book[slot], sizeof(l->book[slot]), book);
  l->id[slot] = id;
  l->peerPct[slot] = peerPct;
  return id != 0;
}

bool kosyncParkedPeerPct(const KosyncParkLedger* l, uint32_t inboxId, double* pct) {
  if (!l || !inboxId) {
    return false;
  }
  for (int i = 0; i < KOSYNC_LEDGER_MAX; i++) {
    if (l->book[i][0] && l->id[i] == inboxId) {
      if (pct) {
        *pct = l->peerPct[i];
      }
      return true;
    }
  }
  return false;
}
