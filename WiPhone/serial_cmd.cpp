#include "serial_cmd.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"
#include "app_books.h"       // booksDebugDumpPage, the `bookpage` command
#include "meshtastic_service.h"   // applyChannelUrl, the `chan` command

#include <Arduino.h>
#include <driver/uart.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ⚠ THE IDF UART API, NOT `Serial`. THIS IS THE WHOLE REASON THE FIRST VERSION READ NOTHING.
 *
 * `setup()` configures UART0 with `uart_param_config()` + `uart_driver_install(UART_NUM_0,
 * ...)` and NEVER CALLS `Serial.begin()`. So the Arduino `HardwareSerial` object is
 * uninitialised: `Serial.available()` always answers 0 and `Serial.read()` never returns a
 * byte, while log output still appears on the port perfectly — because the ESP log path
 * writes to UART0 through the IDF driver, not through `Serial`.
 *
 * That combination is nastily convincing: the port is plainly alive in one direction, so a
 * console built on `Serial` looks like "the phone is ignoring my commands" rather than "this
 * object was never started". Reading through the same driver that owns the port is correct
 * AND unambiguous. The driver is installed with a 2 KB RX ring, so typed input is buffered
 * for us between loop passes.
 */
static const uart_port_t PORT = UART_NUM_0;

/* 512, not 48: `chan <url>` carries a Meshtastic invite URL, and a complete four-channel
 * invite runs ~370 characters. Truncated input is not run (see below), so a small buffer
 * would make the one command this console exists to save you with silently impossible. */
static char     s_buf[512];
static uint16_t s_len = 0;

static void say(const char* fmt, ...) {
  char out[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out, sizeof(out), fmt, ap);
  va_end(ap);
  if (n > 0) {
    uart_write_bytes(PORT, out, (size_t)(n < (int)sizeof(out) ? n : (int)sizeof(out) - 1));
  }
}

static void help() {
  say("\nWiPhone serial commands:\n"
      "  ?          this help\n"
      "  up on      start the WiFi uploader (files land in /roms)\n"
      "  up off     stop the uploader\n"
      "  up         where to point a browser\n"
      "  sync       poll COVEY for mirrored texts now\n"
      "  mirror     mirror poller state\n"
      "  bookpage   dump the open reader page's layout + rendering\n"
      "  chan <url> apply a Meshtastic channel invite URL\n"
      "  chans      list the channels this phone has\n\n");
}

static void reportUploader() {
  if (!gbcXferOn()) {
    say("uploader: off\n");
    return;
  }
  say("uploader: ON  http://%s/  (or http://wiphone.local/)%s\n",
      xferAddr(), xferUsingAP() ? "  [own hotspot]" : "");
  say("          hotspot SSID if used: %s   files added: %d\n",
      xferApName(), xferFilesAdded());
}

static void run(char* line) {
  while (*line == ' ') {
    line++;
  }
  if (!*line || !strcmp(line, "?") || !strcasecmp(line, "help")) {
    help();
    return;
  }

  if (!strcasecmp(line, "up")) {
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up on")) {
    if (gbcXferOn()) {
      say("uploader already on\n");
    } else {
      gbcXferStart();
    }
    reportUploader();
    return;
  }
  if (!strcasecmp(line, "up off")) {
    /* ⚠ Worth taking down rather than leaving up, and not only to save power: while the
     * uploader is on screen the Games app is running, and sipMayPoll() is false — so the
     * SMS mirror poller will not open a socket. Uploading its config and then leaving the
     * uploader up looks exactly like the poller being broken. That is what this was for. */
    if (!gbcXferOn()) {
      say("uploader already off\n");
    } else {
      xferStop();
      say("uploader: off\n");
    }
    return;
  }

  if (!strcasecmp(line, "sync")) {
    if (!smsMirrorPollConfigured()) {
      say("mirror: not configured (%s)\n", smsMirrorPollStatus());
      return;
    }
    smsMirrorPollRequestNow();
    say("mirror: poll requested; watch for a SMSMIRROR line\n");
    return;
  }
  if (!strcasecmp(line, "mirror")) {
    say("mirror: %s (%sconfigured)\n", smsMirrorPollStatus(),
        smsMirrorPollConfigured() ? "" : "not ");
    return;
  }
  if (!strcasecmp(line, "bookpage")) {
    booksDebugDumpPage();
    return;
  }

  /* `chan <url>` — apply a channel invite straight over the cable. Born of a real jam:
   * a chip erase cost the phone its channels, and re-sharing them over the mesh was
   * impossible — the RAK's 2.7 firmware NAKs every DM to this phone with
   * PKI_SEND_FAIL_PUBLIC_KEY (the phone implements no PKC), and an invite broadcast on
   * the primary channel would hand the PSK to everyone in RF range. Serial has neither
   * problem: private by cable, works when the mesh is exactly the broken part. */
  if (!strncasecmp(line, "chan ", 5)) {
    const char* url = line + 5;
    while (*url == ' ') {
      url++;
    }
    int added = meshService.applyChannelUrl(url);
    say("chan: %d channel(s) added\n", added);
    return;
  }
  if (!strcasecmp(line, "chans")) {
    for (int i = 0; i < meshService.getChannelCount(); i++) {
      const MeshChannel* c = meshService.getChannel(i);
      if (c) {
        say("  [%d] '%s' keyLen=%d\n", i, c->name, (int)c->keyLen);
      }
    }
    return;
  }

  say("? unknown command: '%s'  (try ?)\n", line);
}

void serialCmdLoop() {
  uint8_t c;
  // 0 ticks = poll, never block. This runs every main-loop pass.
  while (uart_read_bytes(PORT, &c, 1, 0) == 1) {
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      s_buf[s_len] = '\0';
      if (s_len) {
        run(s_buf);
      }
      s_len = 0;
      continue;
    }
    if (s_len < sizeof(s_buf) - 1) {
      s_buf[s_len++] = (char)c;
    }
    // Over-long input is truncated rather than run in pieces: half a command is not a
    // command, and this is a debug console, not a protocol.
  }
}
