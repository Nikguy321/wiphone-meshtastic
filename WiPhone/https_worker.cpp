/*
 * https_worker.cpp - see https_worker.h. The worker, its queue of job kinds, and the one HTTPS
 * exchange both kinds make (factored out of ai_net.cpp's postOnce/readBody, 0.9.81: the AI's
 * requests go through it exactly as they went before; the weather's add the timed handshake, the
 * second heap check and the tap).
 */
#include "https_worker.h"
#include "gemini.h"             // GEM_NET_*, and the de-chunker (geminiDechunkFeed)
#include "ai_net.h"             // aiRequestActive (netRequestActive)
#include "weather_net.h"        // wxRequestActive (netRequestActive)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <new>
#include <string.h>

#define NET_READ_BUF  1024u

static StaticTask_t      s_tcb;
static StackType_t*      s_stack = NULL;
static TaskHandle_t      s_task  = NULL;
static SemaphoreHandle_t s_go    = NULL;
static char*             s_sink  = NULL;    // NET_SINK_CAP + 1, PSRAM
static uint8_t*          s_raw   = NULL;    // a read off the socket (PSRAM)
static char*             s_dec   = NULL;    // ...de-chunked (PSRAM): never more bytes than were read

/* The queue: one flag a kind (set on the loop, cleared by the worker as it starts the job). */
static volatile bool     s_queued[NET_JOB_KINDS] = { false, false };
static NetJobFn          s_run[NET_JOB_KINDS] = { NULL, NULL };
static volatile int      s_running = -1;

void netHeapNow(uint32_t* freeB, uint32_t* largest, uint32_t* minEver) {
  multi_heap_info_t h;
  heap_caps_get_info(&h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeB)   *freeB   = h.total_free_bytes;
  if (largest) *largest = h.largest_free_block;
  if (minEver) *minEver = h.minimum_free_bytes;
}

bool netHeapOkForHandshake(uint32_t* freeB, uint32_t* largest) {
  uint32_t f = 0, l = 0;
  netHeapNow(&f, &l, NULL);
  if (freeB) *freeB = f;
  if (largest) *largest = l;
  const uint32_t extra = s_stack ? 0u : (uint32_t)NET_STACK_BYTES;
  return l >= NET_CONNECT_LARGEST + extra && f >= NET_CONNECT_FREE + extra;
}

static void workerTask(void*) {
  for (;;) {
    xSemaphoreTake(s_go, portMAX_DELAY);
    for (;;) {
      int k = -1;
      for (int i = 0; i < NET_JOB_KINDS; i++) {      // the AI first: a person is waiting on it
        if (s_queued[i] && s_run[i]) {
          k = i;
          break;
        }
      }
      if (k < 0) {
        break;                                       // back on the semaphore: nothing else on this stack
      }
      s_running = k;
      s_queued[k] = false;
      s_run[k]();                                    // returns, so its locals are destructed
      s_running = -1;
    }
  }
}

bool netWorkerReady(char* why, size_t whyCap) {
  if (!s_sink) {
    s_sink = (char*)heap_caps_malloc(NET_SINK_CAP + 1, MALLOC_CAP_SPIRAM);
    s_raw = (uint8_t*)heap_caps_malloc(NET_READ_BUF, MALLOC_CAP_SPIRAM);
    s_dec = (char*)heap_caps_malloc(NET_READ_BUF, MALLOC_CAP_SPIRAM);
    if (!s_sink || !s_raw || !s_dec) {
      free(s_sink);
      free(s_raw);
      free(s_dec);
      s_sink = NULL;
      s_raw = NULL;
      s_dec = NULL;
      strlcpy(why, "No memory for the request", whyCap);
      return false;
    }
  }
  if (!s_stack) {
    s_stack = (StackType_t*)heap_caps_malloc(NET_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_stack) {
      strlcpy(why, "No internal RAM for the network task - reboot first", whyCap);
      return false;
    }
  }
  if (!s_go) {
    s_go = xSemaphoreCreateBinary();
    if (!s_go) {
      strlcpy(why, "No RAM for the network task", whyCap);
      return false;
    }
  }
  if (!s_task) {
    /* "aiworker": the name `ai`, panicwatch and the bench notes have used since it was the AI's. */
    s_task = xTaskCreateStaticPinnedToCore(workerTask, "aiworker", NET_STACK_BYTES, NULL, 1, s_stack,
                                           &s_tcb, 1);
    if (!s_task) {
      strlcpy(why, "Could not start the network task", whyCap);
      return false;
    }
  }
  return true;
}

bool netWorkerCarved() {
  return s_stack != NULL;
}

void netWorkerSubmit(int kind, NetJobFn run) {
  if (kind < 0 || kind >= NET_JOB_KINDS || !s_go) {
    return;
  }
  s_run[kind] = run;
  s_queued[kind] = true;
  xSemaphoreGive(s_go);
}

int netWorkerRunning() {
  return s_running;
}

bool netWorkerQueued(int kind) {
  return kind >= 0 && kind < NET_JOB_KINDS && s_queued[kind];
}

bool netRequestActive() {
  return aiRequestActive() || wxRequestActive();
}

char* netWorkerSink() {
  return s_sink;
}

uint32_t netWorkerStackFloor() {
  return s_task ? (uint32_t)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)) : 0u;
}

// ── one exchange ─────────────────────────────────────────────────────────────────────────

/* Bytes of body into the sink while they fit (the rest flagged, not kept), every one to the tap. */
static void put(const NetHttpReq* q, NetHttpRes* r, const char* p, size_t n) {
  if (q->tap && n) {
    q->tap(q->tapCtx, p, n);
  }
  size_t room = r->got < q->cap ? q->cap - r->got : 0;
  size_t take = n < room ? n : room;
  if (take) {
    memcpy(q->sink + r->got, p, take);
    r->got += take;
  }
  if (take < n) {
    r->overflow = true;
  }
}

/* The body after the headers: de-chunked by hand, read to its END whatever the cap (an overflow is
 * flagged, its bytes dropped). True with the whole body read; false with r->net saying why. */
static bool readBody(const NetHttpReq* q, NetHttpRes* r, HTTPClient& http, bool chunked, int size) {
  WiFiClient* s = http.getStreamPtr();
  r->got = 0;
  if (!s) {
    r->net = GEM_NET_LOST;
    return false;
  }
  GeminiDechunk d;
  geminiDechunkInit(&d);
  size_t seen = 0;                                   // a Content-Length body's bytes, kept or not
  uint32_t idle = millis();
  for (;;) {
    if (q->cancel && *q->cancel) {
      r->net = GEM_NET_LOST;
      return false;
    }
    const int avail = s->available();
    if (avail > 0) {
      const int want = avail < (int)NET_READ_BUF ? avail : (int)NET_READ_BUF;
      const int n = s->read(s_raw, (size_t)want);
      if (n > 0) {
        idle = millis();
        if (chunked) {
          size_t dn = 0;
          /* A read of n raw bytes de-chunks to at most n: s_dec (NET_READ_BUF) never overflows. */
          if (!geminiDechunkFeed(&d, s_raw, (size_t)n, s_dec, NET_READ_BUF, &dn)) {
            r->net = GEM_NET_BAD_BODY;
            return false;
          }
          put(q, r, s_dec, dn);
          if (d.done) {
            break;
          }
        } else {
          size_t n2 = (size_t)n;
          if (size >= 0 && seen + n2 > (size_t)size) {
            n2 = (size_t)size - seen;                // bytes past Content-Length are not body
          }
          put(q, r, (const char*)s_raw, n2);
          seen += n2;
          if (size >= 0 && seen >= (size_t)size) {
            break;
          }
        }
      }
    } else if (!s->connected()) {
      break;                                         // closed: whole only if the framing says so
    } else if (millis() - idle > q->bodyIdleMs) {
      r->net = GEM_NET_TIMEOUT;
      return false;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));                  // never spin: the loop task shares this core
    }
  }
  if ((chunked && !d.done) || (!chunked && size >= 0 && seen < (size_t)size)) {
    r->net = GEM_NET_LOST;
    return false;
  }
  q->sink[r->got] = '\0';
  return true;
}

/* A refused / failed connect, by WiFiClientSecure::lastError(). */
static int netOfTls(int le) {
  if (le == -0x2700) return GEM_NET_TLS;             // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
  if (le == -0x7F00) return GEM_NET_NOMEM;           // MBEDTLS_ERR_SSL_ALLOC_FAILED
  if (le < -1) return GEM_NET_TLS_OTHER;
  return GEM_NET_CONNECT;                            // refused, unreachable, or the handshake cap
}

int netHttps(const NetHttpReq* q, NetHttpRes* r) {
  memset(r, 0, sizeof(*r));
  r->net = GEM_NET_OK;
  const uint32_t t0 = millis();
  if (!q->sink || !q->rootsPem || !s_raw) {
    r->net = GEM_NET_NOMEM;
    return 0;
  }
  q->sink[0] = '\0';
  IPAddress ip;
  if (!WiFi.hostByName(q->host, ip)) {               // first: "no internet" told apart from a refusal
    r->net = GEM_NET_DNS;
    r->dnsMs = r->ms = millis() - t0;
    return 0;
  }
  r->dnsMs = millis() - t0;
  if (q->heapCheck && !netHeapOkForHandshake(NULL, NULL)) {
    r->net = GEM_NET_NOMEM;                          // the bar again, right before THIS handshake
    r->ms = millis() - t0;
    return 0;
  }
  WiFiClientSecure* c = new (std::nothrow) WiFiClientSecure();
  if (!c) {
    r->net = GEM_NET_NOMEM;
    r->ms = millis() - t0;
    return 0;
  }
  c->setCACert(q->rootsPem);
  int code = 0;
  {
    /* An inner block: ~HTTPClient calls stop() on its client, which must still exist. */
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout((int32_t)q->connectMs);
    http.setTimeout(q->timeoutMs);
    http.setUserAgent(q->userAgent);
    static const char* WANT[] = { "Transfer-Encoding", "Date" };
    http.collectHeaders(WANT, 2);
    /* begin() BEFORE the client connects: on a fresh HTTPClient (_host "") begin(client, url) finds
     * the host "switched" and stop()s a connected client, and sendRequest then handshakes AGAIN -
     * untimed and past the heap bar (arduino-esp32 1.0.6 HTTPClient::beginInternal; review
     * 2026-10-03). Unconnected here, begin() only records the URL, and sendRequest's connect()
     * finds the socket up and rides it. (The client's setTimeout() it then skips is a no-op on
     * WiFiClientSecure in 1.0.6: nothing is lost.) */
    if (!http.begin(*c, q->url)) {
      r->net = GEM_NET_CONNECT;
    } else {
      bool connected = true;
      if (q->connectFirst) {
        const uint32_t th = millis();
        connected = c->connect(q->host, 443, (int32_t)q->connectMs) == 1;
        r->hsMs = millis() - th;
        if (!connected) {
          char eb[64];
          r->tlsErr = c->lastError(eb, sizeof(eb));
          r->net = netOfTls(r->tlsErr);
        }
      }
      if (connected) {
        for (int i = 0; i < 3 && q->hdrName[i]; i++) {
          http.addHeader(q->hdrName[i], q->hdrValue[i] ? q->hdrValue[i] : "");
        }
        code = http.sendRequest(q->method, (uint8_t*)q->body, q->bodyLen);
        netHeapNow(NULL, &r->largestAfterSend, NULL);
        if (code > 0) {
          strlcpy(r->date, http.header("Date").c_str(), sizeof(r->date));
          const bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
          if (!readBody(q, r, http, chunked, http.getSize())) {
            code = 0;
          }
        } else if (code == HTTPC_ERROR_CONNECTION_REFUSED) {
          char eb[64];
          r->tlsErr = c->lastError(eb, sizeof(eb));
          r->net = netOfTls(r->tlsErr);
          code = 0;
        } else if (code == HTTPC_ERROR_READ_TIMEOUT) {
          r->net = GEM_NET_TIMEOUT;
          code = 0;
        } else if (code == HTTPC_ERROR_TOO_LESS_RAM) {
          r->net = GEM_NET_NOMEM;
          code = 0;
        } else if (code == HTTPC_ERROR_ENCODING) {
          r->net = GEM_NET_BAD_BODY;
          code = 0;
        } else {
          r->net = GEM_NET_LOST;
          code = 0;
        }
      }
    }
    http.end();
  }
  c->stop();
  delete c;
  r->code = code;
  r->ms = millis() - t0;
  return code;
}
