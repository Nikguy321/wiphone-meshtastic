/*
Copyright © 2019, 2020, 2021, 2022 HackEDA, Inc.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

#include <dummy.h>

#include <HTTPClient.h>
#include <HTTPUpdate.h>


// TODO:
// - check WiFi before calling => checked so issue#69 is fixed
// - check if using strncpy correctly (does not terminate with a nul)

#include "esp32-hal.h"
#include <stdio.h>
#include "GUI.h"
#include "tinySIP.h"
#include "config.h"
#include "clock.h"
#include "Audio.h"
#include "lwip/api.h"
#include <WiFi.h>
#include "Networks.h"
#include "esp_log.h"
#include <Update.h>
#include "ota.h"
#include "lora.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"   // DIAGNOSTIC: loop-stall watchdog, see setup()
#include "Test.h"
#include "meshtastic_service.h"
#include "music_player.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"   // pulls mirrored texts from COVEY over the LAN
#include "sms_mirror_rx.h"     // smsMirrorTakeNews: the mirror's arrival announcements
#include "serial_cmd.h"        // debug console on USB serial (uploader on/off, mirror sync)
#include "mp3_stream.h"
#include "src/assets/pop_sound.h"

static bool been_in_verify = false;

#ifndef WIPHONE_PRODUCTION
#include "Test.h"
#endif
//#define UDP_SIP   no need this here
extern "C" bool verifyOta() {
  log_d("In verify ota");
  been_in_verify = true;
  return true;
}

static Ota ota("");

GUI gui;
uint32_t chipId = 0;

// Legacy WiPhone RadioHead LoRa is disabled when the Meshtastic PHY owns the radio.
#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
static Lora lora;
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  PERIPHERALS  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

SN7326 keypad(SN7326_I2C_ADDR_BASE, I2C_SDA_PIN, I2C_SCK_PIN);
CW2015 gauge(CW2015_I2C_ADDR, I2C_SDA_PIN, I2C_SCK_PIN);

#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
DRV8833 motorDriver = DRV8833();
#endif

#ifdef USER_SERIAL
HardwareSerial userSerial(2);
int userSerialLastSize = 0;
/* The woods backplate's GPS rides this same UART (GPIO 38 RX / 32 TX) but NOT
 * at the same baud: the GUI path is USER_SERIAL_BAUD, the GPS is gGpsBaud
 * (GPS_SERIAL_BAUD_DEFAULT = 115200, MEASURED — see Hardware.h for why 9600 was
 * wrong and why COVEY's own gps.py had the answer all along). With gGpsNmea on, bytes feed the NMEA reader and the
 * fix lands in meshService; off, the stock user-serial GUI path is untouched,
 * baud included. Persisted as wpmesh/gpsen + wpmesh/gpsbaud; toggled by serial
 * `gps on|off`, retuned by `gps baud <n>`. Default OFF until the plate exists. */
#include "nmea.h"
#include <Preferences.h>
bool       gGpsNmea = false;
uint32_t   gGpsBaud = GPS_SERIAL_BAUD_DEFAULT;
NmeaReader gGpsReader;
/* Set by setup() when the GPS owns the port, cleared by loop()'s first pass once the
 * port has been retuned to gGpsBaud. See the long note at the userSerial.begin() call:
 * the retune CANNOT happen in setup() without risking the INT_WDT panic this avoids. */
bool       gGpsBaudPending = false;

/* The last raw bytes off the wire, for serial `gps raw`. A wrong baud and a
 * module talking binary produce the SAME symptom (bytes climb, sentences stay
 * 0), and only looking at the bytes tells them apart: readable ASCII means the
 * baud is right, 0xB5 0x62 framing means it is speaking UBX, and neither means
 * the rate is still wrong. 64 bytes is under a single NMEA sentence and costs
 * nothing to keep. */
static uint8_t  gGpsRawBuf[64];
static uint8_t  gGpsRawHead = 0;
static uint32_t gGpsRawSeen = 0;

/* Retune the shared UART. Called by `gps on|off`, `gps baud`, the My node
 * toggle, and boot — every path that changes which consumer owns the port. */
void gpsApplyBaud(bool gpsOn) {
  userSerial.updateBaudRate(gpsOn ? gGpsBaud : USER_SERIAL_BAUD);
}

/* Copy the ring out oldest-first. Returns how many bytes landed in `out`. */
int gpsRawSnapshot(uint8_t* out, int cap) {
  int n = (int)(gGpsRawSeen < sizeof(gGpsRawBuf) ? gGpsRawSeen : sizeof(gGpsRawBuf));
  if (n > cap) {
    n = cap;
  }
  int start = (int)(gGpsRawHead + sizeof(gGpsRawBuf) - n) % (int)sizeof(gGpsRawBuf);
  for (int i = 0; i < n; i++) {
    out[i] = gGpsRawBuf[(start + i) % (int)sizeof(gGpsRawBuf)];
  }
  return n;
}
#endif

#ifdef USE_VIRTUAL_KEYBOARD
WiFiUDP *udpKeypad = NULL;
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  I2S AUDIO  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

Audio* audio;

void audio_test() {
  log_e("AUDIO TEST");
  if (audio->start()) {
    log_d("audio: started");
    audio->playRingtone(&SPIFFS);
    //audio->playFile(&SPIFFS, "/ringtone.mp3");
  } else {
    log_e("audio: failed");
  }
}

/* Description:
 *     Setup ringtone playback and vibration motor.
 *     For the ringtone to work, SPIFFS should have two files:
 *         ringtone.mp3
 *             - VERY preferably 16 KHz (or less) audio file
 *         ringtone.ini
 *             - INI file (compatible with NanoINI) which allows three configurations:
 *               - "vibro_on" - time the vibration motor is ON at a time, milliseconds
 *               - "vibro_off" - time the vibration motor is OFF at a time, milliseconds
 *               - "delay" - delay before the vibration motor turn ON for the first time, milliseconds
 *     TODO: generate both files if they are absent
 */
void startRingtone() {

  /* Ringer mode, from Settings > Audio (persisted, loaded at boot by GUI::loadSettings).
   * The motor is driven below regardless of the tone, so "vibrate only" is simply not
   * starting the audio at all — which also avoids turning the codec on for nothing. */
  const uint8_t mode = gui.state.ringerMode;
  const bool playTone = (mode == ControlState::RINGER_RING_AND_VIBRATE);
  const bool doVibrate = (mode != ControlState::RINGER_SILENT);

  if (playTone) {
    // Start audio
    audio->start();

    // Start playing ringtone
    if (!audio->playRingtone(&SPIFFS)) {
      log_d("ERROR: could not play file in SPIFFS");
    }
  } else {
    log_d("ringer: tone suppressed (mode %d)", (int)mode);
  }

  // Initialize vibrating
  gui.state.vibroOn = false;
  gui.state.vibroToggledMs = millis();

  // Default configuration
  gui.state.vibroOnPeriodMs   = 500;
  gui.state.vibroOffPeriodMs  = 2500;
  gui.state.vibroDelayMs      = gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs;
  gui.state.vibroNextDelayMs  = gui.state.vibroDelayMs;

  // Initialize the keyboard LED and motor: both OFF
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);

  // Load vibromotor configuration
  IniFile ini("/ringtone.ini");
  if (ini.load() && !ini.isEmpty()) {
    gui.state.vibroOnPeriodMs  = ini[0].getIntValueSafe("vibro_on", gui.state.vibroOnPeriodMs);
    gui.state.vibroOffPeriodMs = ini[0].getIntValueSafe("vibro_off", gui.state.vibroOffPeriodMs);
    gui.state.vibroDelayMs     = ini[0].getIntValueSafe("delay", gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs);
    gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;

    log_d("vibro on = %d", gui.state.vibroOnPeriodMs);
    log_d("vibro off = %d", gui.state.vibroOffPeriodMs);
    log_d("vibro delay = %d", gui.state.vibroDelayMs);
  }

  // Kickstart ringing
  gui.state.ringing = true;
}

/* Description:
 *     ringtone stops ringing in two cases:
 *       1) user reacted: accepted or declined an incoming call
 *       2) incoming call got cancelled before user reacted
 */
void stopRingtone() {
  audio->shutdown();
  gui.state.ringing = false;
  gui.state.vibroOn = false;
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  HEADPHONE INTERRUPT  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

volatile bool headphoneEvent = false;

void IRAM_ATTR headphoneInterrupt() {
  headphoneEvent = true;
}

// Function that is called after interrupt occurs (not within interrupt)
void headphoneServiceInterrupt() {
#ifdef HEADPHONE_DETECT_PIN
  bool headphones = allDigitalRead(HEADPHONE_DETECT_PIN);
#else
  bool headphones = false;      // TODO: maybe make headphone detect for version 1.3
#endif // HEADPHONE_DETECT_PIN
  log_d("Headphones event = %d", headphones);
  audio->setHeadphones(headphones);
  headphoneEvent = false;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  KEYPAD INTERRUPT & PROCESSING  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

#define KEYBOARD_BUFFER_LENGTH 13         // up to 12 keypresses can be remembered

volatile uint8_t keypadToRead = 0;
uint32_t keypadState = 0;        // 32-bit mask for current state of buttons
// Per-key timestamp of the last "pressed" report. The SN7326 re-reports held
// keys about every 40ms (LONGPRESS_EN), so during gameplay a key that misses
// its heartbeats is a lost release event and gets cleared (stale sweep below).
static uint32_t keyLastSeenMs[32];
/* Per-key timestamp of the last release, for UI debouncing: a "new press" within
 * KEY_BOUNCE_MS of the same key's release is a stale hold re-report, not a human
 * double-tap — humans can't re-tap under ~100ms.
 *
 * ⚠ THIS CLOCK IS TAKEN AT DRAIN TIME, NOT AT EVENT TIME, and the chip carries no
 * timestamps. When several events come out of the FIFO in ONE batch they all read the
 * same millis(), so a release and the press that follows it look SIMULTANEOUS however
 * far apart the finger actually made them — and the press is thrown away as bounce.
 * That is the missed-tap bug (2026-08-22); see releasedThisBatch below, which is the
 * exemption that fixes it. */
static uint32_t keyLastUpMs[32];
/* ⚠ 40 ms UNTIL 2026-08-22, AND IT HAD NEVER ACTUALLY BEEN TESTED. While every release was
 * being applied to CALL this window was armed on the wrong key, so it did nothing for the
 * key being typed; attributing releases correctly armed it FOR THE FIRST TIME. 40 ms was
 * therefore an untested number sitting directly in the path of fast same-key tapping —
 * which is precisely the tentap case. Measured bounces are 6 ms and 13 ms, and no human
 * re-taps a key inside 80 ms, so 25 ms clears the real bounce with margin either side. */
#define KEY_BOUNCE_MS  25u
// Keys the UI considers held down. UI key events are strictly edge-triggered
// off THIS mask: one event per physical press, no matter how long it's held
// and no matter what the chip re-reports meanwhile. Only a real release event
// (or 350ms of heartbeat silence = lost release) re-arms the key. This is
// deliberately separate from keypadState, which other heuristics may clear
// mid-hold (that's what double-clicked and auto-repeated the menus).
static uint32_t uiKeyDown = 0;

/* ── WHY THE KEYPAD IS POLLED AS WELL AS INTERRUPTED ──────────────────────────────────
 * The SN7326's INT is a PULSE, not a level: AUTO_CLEAR_5MS(2) drops it 10 ms after it
 * asserts whether or not anything read the FIFO. The ESP32 side is attached FALLING. So
 * any event the chip produces while INT is ALREADY LOW adds no new edge, and the main
 * loop is never told about it — the event just sits in the FIFO.
 *
 * That is not a rare alignment. A held key re-reports every 40 ms (LONGPRESS_DELAY(1)),
 * each re-report pulsing INT low for 10 ms, so roughly ONE RELEASE IN FOUR lands inside
 * the shadow of the heartbeat before it and is never announced. A lost release leaves
 * uiKeyDown set, and a key whose uiKeyDown is set is DEAF — until the 350 ms stale sweep
 * clears it. A person re-taps in 150–250 ms, which is inside that window. That is
 * exactly what "it misses a tap and I have to tap again" is.
 *
 * The fix is to stop relying on the edge alone: poll the FIFO while input is in flight.
 * Not always — polling an idle phone would cost battery for nothing — but while a key is
 * believed down (which is precisely when a release may be stuck) and for a moment after
 * the last activity. An idle phone still does no I2C at all.
 *
 * ⚠ The game path has polled at 150 ms since the emulator landed, for this same reason.
 * The UI never did, which is why this only ever showed up as "menus miss the odd press".
 */
#define KEYPAD_POLL_MS       40u     // one chip heartbeat
#define KEYPAD_POLL_TAIL_MS  1000u   // keep polling this long after the last event
/* Threshold for "this press is too old to be a hold continuing".
 * ⚠ MEASURED: the chip re-reports a held key about every 109 ms, NOT the 40 ms the
 * LONGPRESS_DELAY(1) comment in SN7326.h claims. 100 ms was therefore BELOW the real
 * heartbeat interval and every heartbeat read as a fresh press. Kept only as a counter
 * now (see the note at its use), but if anything ever acts on it again it must sit well
 * above 109 ms with room for jitter — and below the 350 ms sweep to be worth anything. */
#define KEY_HOLD_GAP_MS      100u
// (named ...Activity, not ...Event: GUI has its own msLastKeypadEvent member for the
// screen-dimming timer and the two are not the same clock.)
static uint32_t msLastKeypadActivity = 0;

/* Input health counters. These exist because the bug above was invisible for two years:
 * a dropped keypress logs nothing and looks like a bad thumb. Read them with `keys` on
 * the serial console. What each one means is written next to where it is incremented. */
static uint32_t kcPollDrained   = 0;   // events the poll recovered that no edge announced
static uint32_t kcBatchRescued  = 0;   // presses saved from the same-batch bounce collapse
static uint32_t kcBounceKilled  = 0;   // presses dropped as bounce (should be near zero)
static uint32_t kcStaleSwept    = 0;   // holds ended by the 350ms sweep = lost releases
static uint32_t kcGapRescued    = 0;   // presses saved from a hold that had silently ended
static uint32_t kcGapMissed     = 0;   // ...and the ones we could not vouch for, still dropped
static uint32_t kcBuffFull      = 0;   // presses dropped because keypadBuff had no room
static uint32_t kcI2cErr        = 0;   // failed reads (each one retries next pass)
static uint32_t kcEmptyPolls    = 0;   // polls that found nothing — the cost of the fix
static uint32_t kcReleaseFixed  = 0;   // releases the chip reported as key 0, re-attributed
static uint32_t kcRelAmbiguous  = 0;   // ...and the ones left alone because 2+ keys were down

/* ── RAW EVENT TRACE ──────────────────────────────────────────────────────────────────
 * The counters say WHAT is going wrong; this says what the CHIP actually sent, which is
 * the only way to settle whether a lost release was never emitted, was emitted and
 * mis-decoded, or was stranded. Each entry is one status byte plus the gap in ms since
 * the one before it. 64 entries is a few seconds of typing — enough to hold a whole
 * "tap, nothing happened, tap again" and its neighbours. Read it with `keys raw`.
 * ⚠ 'P' = the byte carried the PRESSED bit, 'r' = release, '+' = the MORE bit was set,
 * '?' = the code decoded to no key at all (mask 0), which would be its own finding. */
#define KEYTRACE_N 64
static uint8_t  ktByte[KEYTRACE_N];
static uint16_t ktGap[KEYTRACE_N];
static uint8_t  ktFlag[KEYTRACE_N];      // bit0 polled, bit1 unknown-code
static uint8_t  ktHead = 0;
static uint32_t ktLastMs = 0;
static inline void keyTrace(uint8_t b, bool polled, bool unknown, bool dropped = false) {
  (void)dropped;
  const uint32_t nowMs = millis();
  uint32_t d = ktLastMs ? (nowMs - ktLastMs) : 0;
  ktLastMs = nowMs;
  ktByte[ktHead] = b;
  ktGap[ktHead]  = d > 65535 ? 65535 : (uint16_t)d;
  ktFlag[ktHead] = (polled ? 1 : 0) | (unknown ? 2 : 0) | (dropped ? 4 : 0);
  ktHead = (ktHead + 1) % KEYTRACE_N;
}

/* One line of keypad health, for the `keys` serial command and the health log.
 *
 * HOW TO READ IT after a few minutes of ordinary menu use and typing:
 *   drained>0             the INT pulse really is losing events. Theory confirmed; the
 *                         poll is doing the job it was added for.
 *   rescued>0             taps that the drain-time bounce test used to eat. Each one is a
 *                         press the phone would have ignored before this build.
 *   swept  ~0             releases are no longer being lost outright. Before the poll this
 *                         was the count of 350ms deaf spells.
 *   gap    ~0             no evidence of unseen re-presses left over.
 *   killed high           the bounce window is too wide and is eating real taps — lower
 *                         KEY_BOUNCE_MS. Near zero is normal.
 *   full/err >0           the loop is blocking, or the I2C bus is unhappy. Both are their
 *                         own investigation; neither should ever move. */
/* Oldest-first dump of the raw trace. Format per entry: `+123ms 0x4B P +` */
int keypadTrace(char* out, int cap) {
  int n = 0;
  for (int i = 0; i < KEYTRACE_N && n < cap - 1; i++) {
    const uint8_t k = (ktHead + i) % KEYTRACE_N;
    if (!ktGap[k] && !ktByte[k] && !ktFlag[k]) {
      continue;                                   // never written
    }
    n += snprintf(out + n, cap - n, "%s+%ums 0x%02X %c%s%s",
                  n ? "\n" : "", (unsigned)ktGap[k], ktByte[k],
                  (ktByte[k] & SN7326_PRESSED) ? 'P' : 'r',
                  (ktByte[k] & SN7326_MORE) ? " +more" : "",
                  (ktFlag[k] & 2) ? " ?UNKNOWN" : ((ktFlag[k] & 1) ? " (poll)" : ""));
  }
  if (!n) {
    n = snprintf(out, cap, "(nothing recorded - press some keys)");
  }
  return n;
}

int keypadHealth(char* out, int cap) {
  return snprintf(out, cap,
                  "relfix=%lu relamb=%lu killed=%lu gapfix=%lu gapmiss=%lu swept=%lu drained=%lu rescued=%lu full=%lu err=%lu empty=%lu",
                  (unsigned long)kcReleaseFixed, (unsigned long)kcRelAmbiguous,
                  (unsigned long)kcBounceKilled,
                  (unsigned long)kcGapRescued,   (unsigned long)kcGapMissed,
                  (unsigned long)kcStaleSwept,   (unsigned long)kcPollDrained,
                  (unsigned long)kcBatchRescued, (unsigned long)kcBuffFull,
                  (unsigned long)kcI2cErr,       (unsigned long)kcEmptyPolls);
}
/* The boot banner, held until the first health tick can commit it. ⚠ It cannot be written
 * when it is produced: setup() logs the reset reason before the card is known good, so
 * writing there would silently drop the one line that explains a restart. */
static char bootLine[120] = {0};
/* millis() of the last key actually dequeued for the UI. GUI::msLastKeypadEvent is protected
 * and the keypad is drained here anyway, so this is stamped at the source rather than widening
 * GUI's interface for one reader. Used only to hold off the ~1.5 s mesh database save while
 * somebody is scrolling — see MeshtasticService::setUiIdle(). */
static uint32_t gLastKeyMs = 0;

/* Time one named step of the superloop and say so if it blocked. The whole-pass stall detector
 * says THAT a pass froze; this says WHICH call did it. Added chasing the freeze Nick still felt
 * while scrolling after the database save was moved off the input path — the persisted STALL
 * records showed `scr=65 cpu=240 wifi=6`, i.e. screen on, phone active, WiFi already down, so
 * the remaining culprit is somewhere in the reconnect path rather than in storage. */
#define TIME_STEP(name, call)                                                          do {                                                                                   const uint32_t _t0 = millis();                                                       call;                                                                                const uint32_t _d = millis() - _t0;                                                  if (_d > 150) {                                                                        log_e("SLOW STEP: %s took %u ms (wifi=%d) - the whole loop waited on it",                   name, (unsigned)_d, (int)WiFi.status());                                     }                                                                                  } while (0)
// True while the Game Boy emulator app is running. The main loop then skips the
// mesh/LoRa polling so the emulator has the SPI bus and CPU to itself.
volatile bool gGbcActive = false;
// Sticky "pressed since last read" mask for the emulator. Set on every key-down
// here (the main loop sees every press); the emulator ORs it into the held state
// and clears it each frame, so a fast tap isn't missed between its slow polls.
volatile uint32_t gGbcKeyLatch = 0;
RingBuffer<char> keypadBuff(KEYBOARD_BUFFER_LENGTH);        // TODO: maybe used cbuf.h from ESP32 stack?

/* Press a key from somewhere that is not the keypad — the `key` serial command.
 *
 * 🔑 IT PUSHES INTO keypadBuff, WHICH IS THE POINT. Everything downstream — the drain loop,
 * the screen wake, the triple-tap-to-sleep tracker, the easter-egg buffer, the app's own
 * processEvent — then runs EXACTLY as it does for a real press, because it cannot tell the
 * difference. A second dispatch path would be a second thing to keep in step, and the class
 * of bug it would hide is precisely the one that put an untested Photos app in a user's hand.
 *
 * ⚠ Same task as the keypad reader (both run inside loop()), so no locking is needed here —
 * and none would be right anyway: RingBuffer is not reentrant.
 * Returns false if the buffer is full, which is a real answer, not a detail to swallow. */
bool uiInjectKey(char c) {
  if (keypadBuff.full()) {
    return false;
  }
  keypadBuff.put(c);
  return true;
}

void IRAM_ATTR keyboardInterrupt() {
  // This function is intentionally minimal
  // (for example, adding a Serial output here produces ISR crashes)
  keypadToRead = 1;
}

// Function that is called after interrupt occurs (not within interrupt)
// Connect to keypad scanners via I2C and decode the keyboard events into buffer keypadBuff
//
// `polled` = there was no interrupt; we are looking speculatively. See KEYPAD_POLL_MS.
// It changes exactly two things: an empty FIFO must be recognised rather than decoded,
// and anything found this way is counted, because that count is the proof the INT pulse
// is losing events.
void keyboardRead(bool polled) {
  uint8_t key;
  uint32_t mask;
  uint32_t newState = 0;
  char c;
  bool firstRead = true;
  /* How long since we last looked at the chip AT ALL. This is what makes the stale-hold
   * test below trustworthy: a gap in a key's heartbeats only means the key came up if we
   * were actually watching for those heartbeats. */
  static uint32_t msPrevRead = 0;
  const uint32_t sinceLastRead = msPrevRead ? (millis() - msPrevRead) : 0xFFFFFFFFu;
  msPrevRead = millis();
  /* Keys whose RELEASE was decoded earlier in THIS batch. The bounce filter is skipped
   * for them: see the note on keyLastUpMs. A release and a press of the same key coming
   * out of one FIFO read were separated by at least the chip's own hardware debounce
   * (INPUT_FILTER_EN, 6ms+8ms) — the chip does not emit an event for a bounce at all —
   * so they are two real actions, however identical their drain timestamps look. */
  uint32_t releasedThisBatch = 0;
  keypadToRead = 0;           // claim BEFORE reading: an event arriving mid-read
                              // re-raises the flag and gets serviced next pass
  do {
    key = 0;
    sn7326_err_t err = keypad.readKey(key);
    if (err == SN7326_ERROR_BUSY) {
      log_d("i2c reset");
      keypad.reset();         // this seems to resolve rare event of complete hanging
    }
    if (err) {
      kcI2cErr++;
      // Do NOT decode the garbage byte (key==0 decodes as a CALL release) and
      // do NOT abandon whatever is still in the chip's FIFO: the chip auto-
      // clears its INT line, so a dropped event never announces itself again —
      // that was a missed tap, or a button stuck down until the next press.
      // Leave the flag set so the next loop pass retries the read.
      log_d("keypad err code=%d", err);
      keypadToRead = 1;
      break;
    }

    /* ── AN EMPTY FIFO READS BACK AS 0x00, WHICH IS ALSO A REAL EVENT ────────────────
     * 0x00 = key code 0 with the PRESSED bit clear = "CALL released". The chip gives no
     * way to tell that apart from "nothing to report", so a speculative read of an idle
     * chip decodes as a CALL release: it would arm the bounce filter against a genuine
     * CALL press ~every poll, and drive the newState<keypadState reconciliation below
     * with an empty mask, wiping held keys.
     *
     * The disambiguation: a release of a key that is not down is meaningless anyway. So
     * on a POLLED read, a LEADING 0x00 with CALL not held is treated as an empty FIFO.
     * (Leading only — after a byte with MORE set the chip is asserting there is another
     * event, so a 0x00 there is a real CALL release and is decoded normally.)
     *
     * ⚠ This also fixes the game path, which has polled at 150 ms since the emulator
     * landed and has therefore been decoding a phantom CALL release six times a second
     * the whole time. Harmless there only by luck — CALL is not one of the eight keys
     * readPad() maps. */
    if (polled && firstRead && key == 0 &&
        !((keypadState | uiKeyDown) & WIPHONE_KEY_MASK_CALL)) {
      kcEmptyPolls++;
      /* ⚠ TRACED BEFORE IT IS THROWN AWAY. 0x00 is ambiguous — an empty FIFO and a CALL
       * release read identically — and on hardware EVERY key's release came back as 0x00,
       * so this discard is either harmless or it is eating real releases. The trace has to
       * see the bytes this branch drops or it cannot tell those two apart. */
      keyTrace(key, true, false, true);
      return;                 // nothing here: no decode, and nothing to reconcile
    }
    if (polled && firstRead) {
      /* Reached only when a poll found a REAL event — i.e. one the interrupt never
       * announced, because if it had, keypadToRead would have been set and this read
       * would not have been a poll. This counter IS the evidence for the INT-pulse
       * theory: if it stays at 0 through a session of heavy menu use, the theory is
       * wrong and the missed presses are coming from somewhere else. */
      kcPollDrained++;
    }
    firstRead = false;

    // Decode lower 6 bits for character
    switch (key & B111111) {
#ifndef WIPHONE_KEYBOARD
    // 16-key stock keyboard
    case B001011:
      mask = WIPHONE_KEY_MASK_0;
      break;
    case B000000:
      mask = WIPHONE_KEY_MASK_1;
      break;
    case B001000:
      mask = WIPHONE_KEY_MASK_2;
      break;
    case B010000:
      mask = WIPHONE_KEY_MASK_3;
      break;
    case B000001:
      mask = WIPHONE_KEY_MASK_4;
      break;
    case B001001:
      mask = WIPHONE_KEY_MASK_5;
      break;
    case B010001:
      mask = WIPHONE_KEY_MASK_6;
      break;
    case B000010:
      mask = WIPHONE_KEY_MASK_7;
      break;
    case B001010:
      mask = WIPHONE_KEY_MASK_8;
      break;
    case B010010:
      mask = WIPHONE_KEY_MASK_9;
      break;
    case B000011:
      mask = WIPHONE_KEY_MASK_ASTERISK;
      break;
    case B010011:
      mask = WIPHONE_KEY_MASK_HASH;
      break;
    case B011000:
      mask = WIPHONE_KEY_MASK_UP;
      break;
    case B011001:
      mask = WIPHONE_KEY_MASK_BACK;
      break;
    case B011010:
      mask = WIPHONE_KEY_MASK_OK;
      break;
    case B011011:
      mask = WIPHONE_KEY_MASK_DOWN;
      break;
#else
    // 25-key WiPhone keyboard layout
    case B100001:
      mask = WIPHONE_KEY_MASK_0;
      break;
    case B1000:
      mask = WIPHONE_KEY_MASK_1;
      break;
    case B1001:
      mask = WIPHONE_KEY_MASK_2;
      break;
    case B1010:
      mask = WIPHONE_KEY_MASK_3;
      break;
    case B10000:
      mask = WIPHONE_KEY_MASK_4;
      break;
    case B10001:
      mask = WIPHONE_KEY_MASK_5;
      break;
    case B10010:
      mask = WIPHONE_KEY_MASK_6;
      break;
    case B11000:
      mask = WIPHONE_KEY_MASK_7;
      break;
    case B11001:
      mask = WIPHONE_KEY_MASK_8;
      break;
    case B11010:
      mask = WIPHONE_KEY_MASK_9;
      break;
    case B100000:
      mask = WIPHONE_KEY_MASK_ASTERISK;
      break;
    case B100010:
      mask = WIPHONE_KEY_MASK_HASH;
      break;
    case B10:
      mask = WIPHONE_KEY_MASK_UP;
      break;
    case B100100:
      mask = WIPHONE_KEY_MASK_BACK;
      break;
    case B10100:
      mask = WIPHONE_KEY_MASK_OK;
      break;
    case B1:
      mask = WIPHONE_KEY_MASK_DOWN;
      break;

    case B1100:
      mask = WIPHONE_KEY_MASK_LEFT;
      break;
    case B11100:
      mask = WIPHONE_KEY_MASK_RIFHT;
      break;
    case B100:
      mask = WIPHONE_KEY_MASK_SELECT;
      break;
    case B0:
      mask = WIPHONE_KEY_MASK_CALL;
      break;
    case B11:
      mask = WIPHONE_KEY_MASK_END;
      break;
    case B1011:
      mask = WIPHONE_KEY_MASK_F1;
      break;
    case B10011:
      mask = WIPHONE_KEY_MASK_F2;
      break;
    case B11011:
      mask = WIPHONE_KEY_MASK_F3;
      break;
    case B100011:
      mask = WIPHONE_KEY_MASK_F4;
      break;
#endif
    default:
      mask = 0;   // unknown button detected
    }
    keyTrace(key, polled, mask == 0);

    /* ── THE CHIP DOES NOT SAY WHICH KEY WAS RELEASED ────────────────────────────────
     * MEASURED on hardware 2026-08-22 with the raw trace, and this is the whole bug:
     *
     *     +0ms   0x41 P   DOWN pressed
     *     +141ms 0x00 r   ...and the release comes back as key code ZERO
     *     +1047ms 0x54 P  OK pressed
     *     +115ms 0x00 r   ...zero again
     *
     * Key code 0 is CALL in this keymap, so every release in the phone was being applied
     * to CALL: the key you actually pressed never had its bit cleared, and stayed "held"
     * — and therefore DEAF to its own next press — until the 350ms stale sweep. That is
     * the missed menu press and the missed tap while cycling letters, and it has been
     * there since the first commit.
     *
     * It also explains the DOUBLE presses: because keyLastUpMs was stamped for CALL and
     * not for the real key, the contact bounce visible in that same trace (a re-press 6ms
     * after a release) sailed straight through a bounce filter that was watching the
     * wrong key.
     *
     * ⚠ The vendor code half-knew. `newState < keypadState` below is commented "Some
     * buttons were released silently" — a workaround for this very behaviour, which
     * worked only for keypadState and never for uiKeyDown.
     *
     * A release of a key that is not down is meaningless, so attribute it to what IS
     * down. ⚠ INTERRUPT-DRIVEN READS ONLY: an empty FIFO reads back as 0x00 too, and the
     * only thing separating "nothing to report" from "something was released" is that an
     * interrupt fires only when an event really happened. A polled 0x00 is still nothing. */
    if (!polled && !(key & SN7326_PRESSED) && (key & B111111) == 0) {
      const uint32_t held = uiKeyDown | keypadState;
      /* ⚠ EXACTLY ONE KEY DOWN, OR WE DO NOT GUESS. `held & (held - 1)` clears the lowest
       * set bit, so this is the "single bit" test.
       *
       * The first version attributed the release to ALL held keys, on the reasoning that
       * the release branch could clear a multi-bit mask. It can — and that was the bug:
       * releasing ONE of two held keys marked BOTH of them up, so the key still under a
       * finger was no longer in uiKeyDown, and its next ~109 ms heartbeat arrived looking
       * like a brand-new press. That is a PHANTOM KEYPRESS, and it reaches:
       *   - the Select+Back sleep chord — lift one finger a moment before the other and
       *     the other key fires; if the chord had not yet reached SLEEP_CHORD_MS the
       *     screen is still awake and a spurious Back navigates,
       *   - rolling two-key typing (pressing 4 before releasing 3) — duplicate character,
       *   - the Game Boy, where releasing A while holding RIGHT drops RIGHT out of
       *     keypadState for ~109 ms (app_gbc.cpp reads that mask directly, and the
       *     newState<keypadState repair below is skipped in game mode).
       *
       * With two or more keys down the chip has told us something was released and not
       * what, and there is no honest way to pick. So don't: leave it decoded as CALL,
       * exactly as it always was, and let the 350 ms sweep tidy up. That is no worse than
       * the behaviour this whole fix replaced, and it is a great deal better than firing a
       * keypress nobody made. Single-key use — menus, dialling, typing — is unaffected,
       * which is where every reported symptom lived. */
      if (held && !(held & WIPHONE_KEY_MASK_CALL) && (held & (held - 1)) == 0) {
        mask = held;
        kcReleaseFixed++;
      } else if (held & (held - 1)) {
        kcRelAmbiguous++;       // 2+ keys down: deliberately not guessed
      }
    }

    // Decode "pressed/released" bit
    if (key & SN7326_PRESSED) {
      newState |= mask;         // reported (still) pressed — count it even if already known
      uint32_t sinceSeen = 0;
      if (mask) {
        uint8_t b = __builtin_ctz(mask);
        sinceSeen = millis() - keyLastSeenMs[b];         // BEFORE the heartbeat is stamped
        keyLastSeenMs[b] = millis();                     // heartbeat for the stale sweep
      }
      // GAME edge: keypadState and the emulator's sticky latch.
      if (mask && !(keypadState & mask)) {
        keypadState |= mask;
        gGbcKeyLatch |= mask;   // remember the press for the emulator's next poll
        //Serial.print(c); Serial.println(" pressed");
      }

      /* ── UI edge: uiKeyDown ALONE ───────────────────────────────────────────────────
       * ⚠ This USED TO BE NESTED INSIDE the keypadState edge above, which quietly made
       * keypadState a second veto over every UI keypress — and the comment on uiKeyDown
       * ("UI key events are strictly edge-triggered off THIS mask") was therefore false.
       *
       * That nesting is the other half of the missed-tap bug. Lose one release and
       * keypadState keeps the bit; the stale sweep clears uiKeyDown but LEFT KEYPADSTATE
       * SET in the UI, so the next press never even reached the UI code — the key stayed
       * deaf until some release event for it happened to arrive. In practice that means
       * the press after a lost release is ALWAYS swallowed and the one after it works,
       * which is exactly "it misses a tap and I have to tap again".
       *
       * Held-key re-reports are still suppressed, by uiKeyDown, which is what they were
       * always meant to be suppressed by. The sweep now clears both masks together. */
      {
        /* ── THE HOLD THAT ENDED WITHOUT US SEEING IT ──────────────────────────────────
         * A held key re-reports every 40 ms, so a 'pressed' report arriving more than
         * KEY_HOLD_GAP_MS after this key was last seen CANNOT be the continuation of a
         * hold: the heartbeats stopped, which means the key came up and went down again
         * and its release never reached us. Without this the press is vetoed by
         * `uiKeyDown` and the key stays deaf until the 350 ms sweep — one whole tap
         * thrown away. MEASURED on hardware 2026-08-22: gap=20 in a few minutes of menu
         * use, which is precisely the "menu still skips some inputs" Nick was reporting
         * while typing had already come right.
         *
         * ⚠ THIS WAS FIRST SHIPPED AS A COUNTER ONLY, and the caution was real: if the
         * main loop stalls, heartbeats pile up in the chip and a stale gap is OUR fault,
         * not the key's — acting on it would resurrect the menu auto-repeat that
         * uiKeyDown exists to kill. What makes it safe now is `sinceLastRead`: while
         * uiKeyDown is set the loop polls the chip every 40 ms, so if we have looked
         * recently and STILL seen no heartbeat, the key really is up. If we have not
         * looked recently, we say nothing and let the sweep handle it as before. */
        const bool watching = sinceLastRead <= 120;
        if (mask && (uiKeyDown & mask) && sinceSeen > KEY_HOLD_GAP_MS && watching) {
          /* ⚠ COUNTS ONLY — IT USED TO ACT, AND ACTING WAS A BUG. Nick, on hardware:
           * "push OK then push the star key to unlock, it immediately thinks I'm trying to
           * type the star key in the dialer". The trace showed why:
           *
           *     +567ms 0x60 P   * pressed   <- unlocks, correctly swallowed
           *     +109ms 0x60 P   * pressed AGAIN, with no release in between
           *
           * That second report is the chip re-reporting a HELD key, and the interval is
           * ~109 ms — just past the 100 ms threshold this test used. So every heartbeat
           * of a slightly-long press was being promoted into a second keypress.
           *
           * The rescue existed to save a press whose release had been lost. Since releases
           * are attributed correctly (see the code-0 note above) they are not being lost —
           * `swept` sits at 0 — so this has nothing left to save and one clear way to do
           * harm. The 350 ms sweep remains the backstop, and it is safely clear of the
           * heartbeat. Do not re-arm this without re-measuring the heartbeat interval;
           * anything below it turns a hold into a burst of keypresses. */
          kcGapRescued++;
        }

        const bool alreadyDown = mask && (uiKeyDown & mask);
        const bool sameBatchRetap = mask && (releasedThisBatch & mask);
        const bool looksLikeBounce = mask &&
                                     (millis() - keyLastUpMs[__builtin_ctz(mask)] < KEY_BOUNCE_MS);
        bool uiSuppress = mask == 0 || alreadyDown || looksLikeBounce;
        if (sameBatchRetap && !alreadyDown) {
          /* ⚠ OBSERVED ONLY — it does NOT exempt the press from the bounce filter, and
           * that is a deliberate reversal. The exemption was written for a release and a
           * re-press draining together after being stranded in the FIFO; the raw trace
           * then showed nothing is ever stranded (`drained` stayed 0 through whole
           * sessions) and this counter never moved either. What the trace DID show is a
           * genuine contact bounce — a re-press 6ms after a release — which the exemption
           * would have waved straight through as a second menu step. Keeping the count is
           * useful; keeping the exemption would reintroduce the double press. */
          kcBatchRescued++;
        }
        if (looksLikeBounce) {
          kcBounceKilled++;     // real bounce, or a stale hold re-report. Should be rare.
        }
        if (alreadyDown && sinceSeen > KEY_HOLD_GAP_MS) {
          /* Same shape as the rescue above, but we had NOT looked recently enough to
           * trust it, so the press is still being vetoed. If this climbs while `rescued2`
           * stays flat, the poll is not keeping up and KEYPAD_POLL_MS wants lowering. */
          kcGapMissed++;
        }

        /* A press with somewhere to go and nowhere to be put. The buffer holds 13 and the
         * loop drains it every pass, so this should never move; if it does, the loop is
         * blocking long enough to matter and THAT is the finding.
         *
         * It used to be dropped in silence AND latched into uiKeyDown, so nothing ever
         * retried it — the press was simply gone. Leaving the key unlatched instead means
         * the chip's own 40ms re-report delivers it as soon as there is room. */
        const bool noRoom = !uiSuppress && keypadBuff.full();
        if (noRoom) {
          kcBuffFull++;
        }
        if (mask && !noRoom) {
          uiKeyDown |= mask;
        }

        if (!keypadBuff.full() && !uiSuppress) {
          switch (mask) {
          case WIPHONE_KEY_MASK_0:
            c = '0';
            break;
          case WIPHONE_KEY_MASK_1:
            c = '1';
            break;
          case WIPHONE_KEY_MASK_2:
            c = '2';
            break;
          case WIPHONE_KEY_MASK_3:
            c = '3';
            break;
          case WIPHONE_KEY_MASK_4:
            c = '4';
            break;
          case WIPHONE_KEY_MASK_5:
            c = '5';
            break;
          case WIPHONE_KEY_MASK_6:
            c = '6';
            break;
          case WIPHONE_KEY_MASK_7:
            c = '7';
            break;
          case WIPHONE_KEY_MASK_8:
            c = '8';
            break;
          case WIPHONE_KEY_MASK_9:
            c = '9';
            break;
          case WIPHONE_KEY_MASK_ASTERISK:
            c = '*';
            break;
          case WIPHONE_KEY_MASK_HASH:
            c = '#';
            break;
          case WIPHONE_KEY_MASK_UP:
            c = WIPHONE_KEY_UP;
            break;
          case WIPHONE_KEY_MASK_BACK:
            c = WIPHONE_KEY_BACK;
            break;
          case WIPHONE_KEY_MASK_OK:
            c = WIPHONE_KEY_OK;
            break;
          case WIPHONE_KEY_MASK_DOWN:
            c = WIPHONE_KEY_DOWN;
            break;

          case WIPHONE_KEY_MASK_LEFT:
            c = WIPHONE_KEY_LEFT;
            break;
          case WIPHONE_KEY_MASK_RIFHT:
            c = WIPHONE_KEY_RIGHT;
            break;
          case WIPHONE_KEY_MASK_SELECT:
            c = WIPHONE_KEY_SELECT;
            break;
          case WIPHONE_KEY_MASK_CALL:
            c = WIPHONE_KEY_CALL;
            break;
          case WIPHONE_KEY_MASK_END:
            c = WIPHONE_KEY_END;
            break;
          case WIPHONE_KEY_MASK_F1:
            c = WIPHONE_KEY_F1;
            break;
          case WIPHONE_KEY_MASK_F2:
            c = WIPHONE_KEY_F2;
            break;
          case WIPHONE_KEY_MASK_F3:
            c = WIPHONE_KEY_F3;
            break;
          case WIPHONE_KEY_MASK_F4:
            c = WIPHONE_KEY_F4;
            break;

          default:
            c = 0;
          }
          if (c) {
            keypadBuff.put(c);
          }
        }
      }
    } else {
      if (mask) {   // unconditional: the state bit may have been cleared mid-hold
        keypadState &= ~mask;
        uiKeyDown &= ~mask;                            // re-arm the UI edge
        // Loop, not ctz: an attributed code-0 release above can carry several held keys.
        for (uint32_t m = mask; m; m &= m - 1) {
          keyLastUpMs[__builtin_ctz(m)] = millis();    // for the UI debounce window
        }
        releasedThisBatch |= mask;                     // exempt a re-press in THIS batch
        //Serial.print(c); Serial.println(" released");
      }
    }
  } while (key & SN7326_MORE);       // decode "more" bit

  // Some buttons were "released" silently.
  // NOT during gameplay: this batch only lists keys with NEW events, so pressing
  // (or releasing) button A while HOLDING right produced a batch without "right"
  // and wiped the held key -> held-direction games went run/stop/run. Releases
  // do arrive as real events above, so the held mask stays correct without this.
  if (!gGbcActive && newState < keypadState) {
    keypadState = newState;
  }

  // Something real came out of the chip. Keeps the poll alive for a moment afterwards
  // (KEYPAD_POLL_TAIL_MS) so a stuck event has something to find it.
  if (!firstRead) {
    msLastKeypadActivity = millis();
  }
}

#ifdef USE_VIRTUAL_KEYBOARD
void keyboardUdpRead() {
  if (udpKeypad && udpParsePacketSafe(*udpKeypad) > 0) {
    char buff[1000];
    int cb = udpKeypad->read(buff, sizeof(buff) - 1);
    buff[cb] = 0;
    //log_d("Keypad received: %s", buff);
    for (int i = 0; i < cb && !keypadBuff.full(); i++) {
      if (buff[i] == 10 || buff[i] == 13 || !buff[i]) {
        continue;
      }
      keypadBuff.put(buff[i]);
    }
  }
}
#endif

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  POWER/END BUTTON INTERRUPT  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

// Interrup routines to check for the power button presses, which is not connected to keypad scanner, but via a dedicated GPIO extender pin

bool powerButtonPressed = false;
bool poweringOff = false;
volatile bool gpioExtenderEvent = false;

void IRAM_ATTR gpioExtenderInterrupt() {
  // This function is intentionally minimal
  gpioExtenderEvent = true;
}

// Function that is called after interrupt occurs (not within interrupt)
bool gpioExtenderServiceInterrupt() {
  gpioExtenderEvent = false;
  bool powerButton = gpioExtender.digitalRead(POWER_CHECK & ~EXTENDER_FLAG) == LOW;
  //log_d("powerButton = %d", powerButton);
  if (powerButton != powerButtonPressed) {
    powerButtonPressed = powerButton;
    if (powerButton) {
      keypadBuff.put(WIPHONE_KEY_END);
    }
    return true;
  }
  return false;
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  SETUP  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

void setup() {
  // Initialize serial
  const uart_config_t uart_config = {
    .baud_rate = SERIAL_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
  };

  int RX_BUF_SIZE =  1024;

  uart_param_config(UART_NUM_0, &uart_config);
  uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

  for(int i=0; i<17; i=i+8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  log_i("\r\nChip id: %X %d %d", chipId, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));
  log_i("Firmware version: %s", FIRMWARE_VERSION);

  // Initialize I2C and wake up battery gauge first
  gauge.connect();
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gui.state.gaugeInited = gauge.configure();
  log_d("\r\nBattery gauge: %s\n", gui.state.gaugeInited ? "OK" : "FAILED");
  delay(10);
#endif // WIPHONE_BOARD || WIPHONE_INTEGRATED

  log_v("Free memory after gauge: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize GPIO extender
#ifdef WIPHONE_INTEGRATED_1_4
  if (gpioExtender.begin()) {
    log_v("extender succ");
    gui.state.extenderInited = true;
    // Input
    allPinMode(POWER_CHECK, INPUT);
    allPinMode(TF_CARD_DETECT_PIN, INPUT);
    allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
    // Output
    allPinMode(KEYBOARD_LED, OUTPUT);
    allPinMode(VIBRO_MOTOR_CONTROL, OUTPUT);
    allPinMode(POWER_CONTROL, OUTPUT);
    // Default state
    allDigitalWrite(POWER_CONTROL, LOW);
    allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
    allDigitalWrite(KEYBOARD_LED, HIGH);
  } else {
    log_e("extender failed");
    gui.state.extenderInited = false;
  }
#else // not WIPHONE_INTEGRATED_1_4
#ifdef WIPHONE_INTEGRATED_1_3
  {
    auto err = gpioExtender.config(
                 // Input pins
                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,
                 // Output HIGH
                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1 | EXTENDER_PIN_FLAG_B7
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#else // not WIPHONE_INTEGRATED_1_3
#ifdef WIPHONE_INTEGRATED_1
  {
    auto err = gpioExtender.config(
                 // Input pins
                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,
                 // Output HIGH
                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A1 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gui.state.extenderInited = (err == SN7325_ERROR_OK);
    err = gpioExtender.setInterrupts(EXTENDER_PIN_FLAG_A2);     // POWER_OFF interrupt
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#endif // WIPHONE_INTEGRATED_1
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4

  log_v("Free memory after integrated: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Fast test of PSRAM presence
  void* p = heap_caps_malloc(100000, MALLOC_CAP_SPIRAM);
  if (p != NULL) {
    gui.state.psramInited = true;
    freeNull((void **) &p);
  }

  log_v("Free memory after psram: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize power
#if defined(POWER_CONTROL) && POWER_CONTROL >= 0
  allPinMode(POWER_CONTROL, OUTPUT);
  allDigitalWrite(POWER_CONTROL, LOW);
#endif

  log_v("Free memory after power: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if defined(POWER_CHECK) && POWER_CHECK >= 0
#ifdef WIPHONE_INTEGRATED_1_4
  log_d("enabling interrupt (rev1.4)");
  gpioExtender.enableInterrupt(2, FALLING);
#else // not WIPHONE_INTEGRATED_1_4
#ifdef WIPHONE_INTEGRATED_1_3
  log_d("enabling interrupt input (rev1.3)");
  allPinMode(POWER_CHECK, INPUT_PULLUP);
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4
#endif // defined(POWER_CHECK) && POWER_CHECK >= 0

  Random.feed(micros());      // TODO: maybe feed microphone data to Random.feed()

  // Mounter internal filesystem
  if (SPIFFS.begin()) {
    log_d("SPI filesystem mounted");
    // Install/refresh the Meshtastic notification "pop" sound (embedded in
    // firmware so no SPIFFS re-flash is needed). Rewritten on each boot so
    // firmware updates to the sample always take effect.
    {
      File pf = SPIFFS.open("/pop.pcm", "w");
      if (pf) {
        pf.write(pop_pcm, sizeof(pop_pcm));
        pf.close();
        log_d("Installed /pop.pcm (%u bytes)", (unsigned)sizeof(pop_pcm));
      }
    }
  } else {
    log_d("SPI filesystem mount FAILED");
  }

  log_v("Free memory after internal fs: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize GUI
  log_d("Initializing screen");
  gui.init(lcdLedOnOff);
  gui.redrawScreen(false, false, true);   // only screen

  log_v("Free memory after gui: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if LCD_LED_PIN >= 0
  // Turn on backlight
  log_d("LCD_LED_PIN = %d", LCD_LED_PIN);
  allPinMode(LCD_LED_PIN, OUTPUT);
#if GPIO_EXTENDER == 1509
  gpioExtender.ledDriverInit(LCD_LED_PIN ^ EXTENDER_FLAG);
#else
  allPinMode(LCD_LED_PIN, OUTPUT);
#endif // GPIO_EXTENDER == 1509
  gui.toggleScreen();
#endif

  log_v("Free memory after lcd led: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Mount SD card and SPIFFS (AFTER the screen & SPI initialization)
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)

  // Init SD card detection pin
  allPinMode(TF_CARD_DETECT_PIN, INPUT);

  // Initilize hardware serial:
  gui.state.battVoltage = gauge.readVoltage();
  gui.state.battSoc = gauge.readSocPrecise();
  log_d("Voltage = %.2f", gui.state.battVoltage);
  log_d("SOC = %.1f", gui.state.battSoc);
#endif // WIPHONE_BOARD || WIPHONE_INTEGRATED

  log_v("Free memory after sd spiffs: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // If voltage is extremely low, power off immediately
  if (gui.state.battVoltage < 3.1) {
    powerOff();
    gui.processEvent(millis(), POWER_OFF_EVENT);
  }

  // INITIALIZE OTHER I2C DEVICES

  // Battery gauge
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gauge.showVersion();
#endif // WIPHONE_BOARD

  // Mount SD card

  if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_FREQUENCY)) {
    log_d("Card mounted");
  } /*else {
    log_d("Card mount FAILED");
  }*/

  /* ⚠ THE WALLPAPER IS LOADED HERE, AND NOT ONLY IN gui.init(), AND THE ORDER ABOVE IS WHY.
   * gui.init() runs ~50 lines earlier — before the line above — so the SD.exists() inside it
   * asks an UNMOUNTED filesystem and always gets false. The Photos app writes the chosen
   * wallpaper to /background.jpg on the SD card, so until this second call existed, "Set as
   * wallpaper" wrote a perfectly good file that nothing ever read: the phone fell back to the
   * compiled-in default on every boot and the user saw no change and no error (Nick,
   * 2026-08-25). Moving SD.begin() earlier is NOT the fix — it shares SPI with the screen and
   * the comment above says that order is deliberate. Ask the phone with `wallpaper`. */
  gui.loadWallpaper();

  /* ⚠ SEED cardPresent HERE, OR EVERY CARD-GATED PATH LIES FOR THE FIRST MINUTE.
   * It is declared `false` in GUI.h and was assigned in exactly one place: the
   * once-a-minute battery tick. So for up to BATTERY_CHECK_PERIOD_MS after every boot
   * the phone believed it had no card. healthDump() bails on `!cardPresent`, which
   * means `health` answered "nothing to read - no card, or no /health.log yet" WITH A
   * PERFECTLY GOOD CARD SEATED — measured 2026-08-23, when the file it was denying held
   * 926 samples and returned in full 80 s later. That is exactly the window in which
   * someone plugs a phone into the cable and asks it what happened.
   * The pin is configured by the allPinMode() above, and gpioExtender.begin() ran far
   * earlier in setup(), so it is readable by the time we get here. */
#if TF_CARD_DETECT_PIN >= 0
  gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#endif


  // Initialize keypad
  {
    //keypad.connect();     // sets I2C speed to 400 kHz (as per datasheet)
    sn7326_err_t err = keypad.config();
    if (err != SN7326_ERROR_OK) {
      log_d("keypad error = %d", err);
    }
    gui.state.scannerInited = (err == SN7326_ERROR_OK);
  }

  log_v("Free memory after keypad: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Initialize RMT peripheral to enable the amplifier
#ifdef WIPHONE_INTEGRATED_1_4
  rmtTxInit(AMPLIFIER_SHUTDOWN, false);
#else
#ifdef WIPHONE_INTEGRATED_1_3
  // ?
#endif // WIPHONE_INTEGRATED_1_3
#endif // WIPHONE_INTEGRATED_1_4

  // Initialize audio systems (hardware codec and I2S peripheral)
#ifdef I2S_MCLK_GPIO0
  {
    // Use GPIO_0 for providing MCLK to the audio codec IC
    //SET_PERI_REG_BITS(PIN_CTRL, CLK_OUT1, 0, CLK_OUT1_S);       // esp-adf
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1); // esp-adf
    REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 0);                         // Source: https://esp32.com/viewtopic.php?t=1521
  }
#endif // I2S_MCLK_GPIO0


  log_v("Free memory after config files: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // GPIOs

  /* ⚠ allPinMode, NOT pinMode: BATTERY_CHARGING_STATUS_PIN is EXTENDER_PIN(0) == 64 and the
   * ESP32 has GPIO 0-39, so plain pinMode() configured a pin that does not exist and this
   * line did nothing at all. Benign only because the real configuration happens correctly
   * via allPinMode() in the extender block earlier in setup() — but a line that looks like
   * it sets up the charge-status input, and does not, is exactly how the sibling bug in the
   * battery tick survived so long (see the note there). BATTERY_PPR_PIN is a genuine GPIO
   * (37) and is fine either way; it goes through the same accessor for consistency. */
#ifdef WIPHONE_INTEGRATED
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  allPinMode(BATTERY_PPR_PIN, INPUT);
#endif // WIPHONE_INTEGRATED
#ifdef WIPHONE_BOARD
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  //pinMode(USB_POWER_DETECT_PIN, INPUT);
#endif // WIPHONE_BOARD

#if defined(KEYBOARD_RESET_PIN) && KEYBOARD_RESET_PIN > 0
  pinMode(KEYBOARD_RESET_PIN, OUTPUT);         // keypad RST
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);      // no RST
  delay(1);                    // 1 ms
  digitalWrite(KEYBOARD_RESET_PIN, LOW);       // RST
  delay(1);                    // 1 ms
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);      // no RST
#endif

  pinMode(KEYBOARD_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(KEYBOARD_INTERRUPT_PIN), keyboardInterrupt, FALLING);
  pinMode(GPIO_EXTENDER_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(GPIO_EXTENDER_INTERRUPT_PIN), gpioExtenderInterrupt, FALLING);


#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
  motorDriver.attachMotorA(AIN1, AIN2);
  motorDriver.attachMotorB(BIN1, BIN2);
  pinMode(MotorEN , OUTPUT);
  digitalWrite(MotorEN , LOW);
#endif

  //esp_pm_config_esp32_t conf = { RTC_CPU_FREQ_240M, 240, RTC_CPU_FREQ_80M, 10, true };
//  esp_pm_config_esp32_t conf = { RTC_CPU_FREQ_240M, RTC_CPU_FREQ_80M, true };
//  esp_err_t err;
//  err = esp_pm_configure((const void*) &conf);
//  log_d("Power management: %d", (int) err);

#ifdef USER_SERIAL
  //allDigitalWrite(EXTENDER_PIN_B0, HIGH);     // TODO: why do we do this?
  {
    Preferences p;
    p.begin("wpmesh", true);
    gGpsNmea = p.getBool("gpsen", false);
    gGpsBaud = p.getUInt("gpsbaud", GPS_SERIAL_BAUD_DEFAULT);
    p.end();
  }
  /* 🛑 ALWAYS OPEN AT USER_SERIAL_BAUD, EVEN WHEN THE GPS OWNS THIS PORT, AND RETUNE
   * FROM loop() INSTEAD. This used to open straight at gGpsBaud, and the comment here
   * used to say that retuning afterwards "would work too" but dirtied the reader's
   * counters on every boot. It does not merely dirty them — opening at 115200 here
   * CRASHED THE PHONE, and this is the fix for it.
   *
   * 🔑 MEASURED, 2026-08-25, from three reproduction boots with the plate fitted:
   *      Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)
   *      setup() -> HardwareSerial::begin -> uartBegin -> uartAttachRx
   *             -> uartEnableInterrupt -> esp_intr_alloc -> vTaskExitCritical
   *             -> _xt_lowint1 -> _uart_isr
   * The woods plate's GPS starts talking the moment it has POWER; it does not wait for
   * us. So by the time begin() installs the RX interrupt handler the line is already
   * saturated at 115200, the ISR fires inside the attach's own critical section, and
   * the CPU1 interrupt watchdog times out. It then reboots into setup() and does it
   * again — a boot loop that presents as a phone with a dark screen and dead buttons.
   *
   * ⚠ THE RATE IS THE WHOLE POINT, not the baud being "wrong": at USER_SERIAL_BAUD the
   * same live line still interrupts us, but ~12x less often (9600/10 vs 115200/10 bytes
   * a second), and that attach survives — it is what every gpsen=false boot has always
   * done. The retune afterwards calls updateBaudRate(), which does NOT re-enter
   * esp_intr_alloc, so it cannot reproduce the fault. That asymmetry is exactly why
   * toggling `gps on` by hand always worked while booting with it on did not.
   *
   * The counters still start clean: gGpsReader.reset() runs at the retune, below. */
  userSerial.begin(USER_SERIAL_BAUD, USER_SERIAL_CONFIG,
                   USER_SERIAL_RX, USER_SERIAL_TX);
  if (gGpsNmea) {
    gGpsBaudPending = true;      // loop() finishes the job, once, on its first pass
  }
#endif


  /* ── WHY DID IT RESTART? ──────────────────────────────────────────────────────────
   * The chip records this across a reboot and it is the difference between guessing and
   * knowing. The ones that matter here:
   *   1  POWERON    the switch, or the battery was pulled
   *   3  SW         esp_restart() -- our own reboot menu item
   *   4  PANIC      a crash: null deref, abort(), assert
   *   5  INT_WDT    an interrupt watchdog -- something blocked with interrupts off
   *   6  TASK_WDT   a task watchdog -- the main loop stopped feeding it
   *   7  WDT        other watchdog
   *   9  BROWNOUT   THE SUPPLY SAGGED. Not software at all -- a tired battery under a
   *                 WiFi transmit peak does this, and it looks exactly like a crash.
   *  10  RTC/SDIO
   * Printed with log_e so it survives into the field log, where log_d does not. */
  log_e("BOOT: reset_reason=%d heap=%u psram=%u",
        (int)esp_reset_reason(), ESP.getFreeHeap(), ESP.getFreePsram());
  /* ⚠ THE BUILD STAMP IS THE POINT OF THIS LINE, not the heap numbers.
   * /health.log records only `up=` minutes — no wall clock, and until now nothing saying WHICH
   * FIRMWARE wrote a sample. On 2026-08-24 that gap made a yes/no question unanswerable from
   * four hours of data: `chg=` had just been fixed to read the right chip, the log contained a
   * long run of chg=1 that tracked discharging perfectly, and there was NO WAY TO TELL whether
   * those samples came from before the fix (when the flag was reading GPIO 32, a UART TX line
   * that idles high) or after it. Both readings fit and neither could be ruled out.
   * __DATE__/__TIME__ is the compiler's stamp, so every build is distinguishable and every run
   * of samples can be attributed to the firmware that produced it. */
  snprintf(bootLine, sizeof(bootLine), "BOOT reset_reason=%d heap=%u psram=%u build=" __DATE__ " " __TIME__,
           (int)esp_reset_reason(), ESP.getFreeHeap(), ESP.getFreePsram());
  printf("\r\nBooting...\r\n");

  uint8_t mac[6];
  wifiState.getMac(mac);

  nvs_stats_t nvs_stats;
  nvs_get_stats(NULL, &nvs_stats);
  log_d("NVS stats: UsedEntries = %d, FreeEntries = %d, AllEntries = %d\n",
        nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries);

  gui.loadSettings();
  gui.reloadMessages();

  log_v("Free memory after reload messages: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  if (gui.state.dimming || gui.state.sleeping) {
    uint32_t now = millis();
    if (gui.state.doDimming()) {
      gui.state.scheduleEvent(SCREEN_DIM_EVENT, now + gui.state.dimAfterMs*2);
    }
    if (gui.state.doSleeping()) {
      gui.state.scheduleEvent(SCREEN_SLEEP_EVENT, now + gui.state.sleepAfterMs*2);
    }
  }

  log_v("Free memory after gui sleeping: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  // Set thread priorities (NOTE: in FreeRTOS, tasks with higher priorities will always be exectuted first, unless they have nothing to do)
  uint32_t mainThreadPrio = ESP_TASK_TCPIP_PRIO >> 1;
  if (tskIDLE_PRIORITY >= mainThreadPrio) {
    mainThreadPrio = tskIDLE_PRIORITY + 1;
  }
  log_i("lwIP thread priority: %d", ESP_TASK_TCPIP_PRIO);
  log_i("Main loop thread priority: %d", mainThreadPrio);
  log_i("Idle task priority: %d", tskIDLE_PRIORITY);
  log_i("Tick period: %d ms", portTICK_PERIOD_MS);
  vTaskPrioritySet(NULL, mainThreadPrio);

  log_v("Free memory after vTaskPrioritySet: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


  wifiState.init();

  log_v("Free memory after wifi init: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  if (!wifiState.hasPreferredSsid()) {
    wifiState.disable();  // if we don't have a saved SSID to connect to, turn off WiFi to save power
  }

  int counter = 0;
  wifiState.loadPreferred();

  if (!wifiState.userDisabled()) {
    wifiState.connectToPreferred();
    while (!wifiState.isConnected() && ++counter < 10) {
      log_v("Waiting for wifi: %d %d", counter, ESP.getFreeHeap());
      wifiState.connectToPreferred();
      delay(500);
    }
  }

#if OTA_TRANSPORT_AVAILABLE
  /* ⚠ updateExists() is the NETWORK call — it is what opens the TLS connection. It must stay
   * to the RIGHT of the cheap local predicates, because && short-circuits left to right. It
   * used to sit to their LEFT, so the handshake ran on every boot whatever the settings said,
   * and autoUpdateEnabled() defaults to true so the gate would not have saved it either. */
  if (!ota.hasJustUpdated() && ota.userRequestedUpdate()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  } else if (!ota.hasJustUpdated() && (ota.autoUpdateEnabled() || ota.userRequestedUpdate()) && ota.updateExists()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  }

  ota.setUserRequestedUpdate(false);
#endif // OTA_TRANSPORT_AVAILABLE

  static Audio audio_local(true, I2S_BCK_PIN, I2S_WS_PIN, I2S_MOSI_PIN, I2S_MISO_PIN);
  audio = &audio_local;
  gui.state.codecInited = !audio->error();

  // Load phone configs
  {
    CriticalFile ini(Storage::ConfigsFile);
    if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
      if (ini[0].hasKey("v") && !strcmp(ini[0]["v"], "1")) {    // check version of the file format

        // Load audio volume
        if (ini.hasSection("audio")) {
          int8_t speakerVol, headphonesVol, loudspeakerVol;
          audio->getVolumes(speakerVol, headphonesVol, loudspeakerVol);      // default values
          speakerVol = ini["audio"].getIntValueSafe("speaker_vol", speakerVol);
          headphonesVol = ini["audio"].getIntValueSafe("headphones_vol", headphonesVol);
          loudspeakerVol = ini["audio"].getIntValueSafe("loudspeaker_vol", loudspeakerVol);
          audio->setVolumes(speakerVol, headphonesVol, loudspeakerVol);
          log_d("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
          log_i("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
        }

        // Load timezone config
        if (ini.hasSection("time")) {
          float tz = ini["time"].getFloatValueSafe("zone", 0);
          ntpClock.setTimeZone(tz);
        }

        // Load screen dimming & sleeping config
        if (ini.hasSection("screen")) {
          gui.state.brightLevel = ini["screen"].getIntValueSafe("bright_level", 100);
          /* ⚠ DEFAULT ON. These two fallbacks used to be 0, which disagreed with the
           * missing-SECTION branch a dozen lines below (it defaults both TRUE) and with
           * every expectation of a phone. A config that simply lacks the key got a screen
           * that never dims and never sleeps - full backlight until the battery is flat,
           * silently. The shipped data/configs.ini had the same two zeros, so any
           * `pio run -t uploadfs` handed a device that state. Found in the 2026-08-22
           * battery audit. Both are now ON, matching the else-branch. */
          gui.state.dimming = ini["screen"].getIntValueSafe("dimming", 1) > 0;
          gui.state.dimLevel = ini["screen"].getIntValueSafe("dim_level", 15);
          gui.state.dimAfterMs = ini["screen"].getIntValueSafe("dim_after_s", 20)*1000;
          gui.state.sleeping = ini["screen"].getIntValueSafe("sleeping", 1) > 0;
          gui.state.sleepAfterMs = ini["screen"].getIntValueSafe("sleep_after_s", 30)*1000;
          gui.state.screenBrightness = gui.state.brightLevel-1; // Forces the new brightness setting to be applied
          gui.processEvent(0, 0); // Need to call gui event loop so brightness settings are applied
        } else {
          gui.state.brightLevel = 100;
          gui.state.dimming = true;
          gui.state.dimLevel = 15;
          gui.state.dimAfterMs = 20000;
          gui.state.sleeping = true;
          gui.state.sleepAfterMs = 30000;
        }

        /* ⚠ DEFAULT ON, AND FOR THE SAME REASON THE TWO ABOVE ARE. A missing SECTION has
         * always defaulted this to 1 (the `: 1` below); a section that merely lacks the KEY
         * defaulted to 0 — so two configs that both say nothing about locking gave opposite
         * answers, and the quieter one silently shipped a phone whose screen never locked.
         * That is exactly the disagreement found in [screen] during the 2026-08-22 battery
         * audit, where it produced a phone that never dimmed and never slept. Same shape,
         * same fix: the two branches now agree. (Both phones read 1 today, so this was NOT
         * the cause of the 2026-08-25 "phone 2 never locks" — that was GUI::inCall(). This
         * is the landmine beside it.) */
        gui.state.locking = ini.hasSection("lock") ? ini["lock"].getIntValueSafe("lock_keyboard", 1) : 1;
      }
    }
    gui.setAudio(audio);
  }

#ifdef HEADPHONE_DETECT_PIN
  pinMode(HEADPHONE_DETECT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HEADPHONE_DETECT_PIN), headphoneInterrupt, CHANGE);
  bool headphones = allDigitalRead(HEADPHONE_DETECT_PIN);
  audio->setHeadphones(headphones);
#endif // HEADPHONE_DETECT_PIN

  log_v("Free memory after audio: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  ntpClock.startUpdates();

  // Setup for LoRa messaging
#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
  lora.setup();
#endif

  // Meshtastic background service (owns the LoRa radio when MESHTASTIC_PHY is set)
  meshService.setup();

  log_d("WiPhone, firmware date = " __DATE__);

#ifdef CONFIG_APP_ROLLBACK_ENABLE
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    log_i("Got partition state: %d %d %d", ota_state, ESP_OTA_IMG_PENDING_VERIFY, been_in_verify);
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      log_i("Committing update");
      ota.commitUpdate();
    }
  }

#endif

  /* ── DIAGNOSTIC (2026-08-25): SUBSCRIBE THE LOOP TASK TO THE TASK WATCHDOG ────────────
   * 🔑 Nothing has ever watched this task, which is why the GPS wedge is SILENT: the loop
   * stops, other FreeRTOS tasks keep printing, and no watchdog notices. The existing
   * LOOP STALL detector (see loop()) cannot see it either — that one measures the gap
   * between two passes, so it only reports a stall the loop RECOVERED from. A stall that
   * never ends produces nothing at all.
   *
   * ⚠ panic=false ON PURPOSE. A timeout then PRINTS the offending task and its backtrace
   * and lets the phone carry on, instead of rebooting and destroying the evidence. That
   * backtrace is the entire point of this build.
   *
   * ⚠ 20 s, not the 5 s default: WiFi.disconnect(true) is measured at ~5 s of blocking in
   * this firmware (docs/HANDOFF.md), and the boot path has legitimate 951 ms passes. A
   * threshold under those would cry wolf and teach us to ignore it.
   *
   * ⚠ Arduino feeds this watchdog once per loop() RETURN — and loop() here never returns,
   * it wraps its own while(1). So the feed has to live inside that loop; it does. */
  esp_task_wdt_init(20, false);
  esp_task_wdt_add(NULL);            // NULL = this task = loopTask (setup runs on it)
  log_e("WDT: loop task subscribed, 20 s, print-not-panic (diagnostic build)");

  printf("\r\nBooted\r\n");

  /* ── MP3_HEAP_PROBE ────────────────────────────────────────────────────────────────
   * Allocates and frees the MP3 decoder once and reports what it cost.
   *
   * This checks the riskiest claim in the music player: that helix's ~29 KB lands in
   * PSRAM and leaves the INTERNAL heap alone. If that is ever wrong the phone does not
   * fail here — it fails minutes later when the WiFi PHY cannot get 2 KB for RF
   * calibration and phy_init aborts, with a backtrace pointing nowhere near audio. That
   * is a whole debugging session, so it is worth one line of proof.
   *
   * Off by default. Build with -DMP3_HEAP_PROBE, read at 500000 baud, then turn it off. */
#ifdef MP3_HEAP_PROBE
  {
    const uint32_t i0 = ESP.getFreeHeap(), b0 = ESP.getMaxAllocHeap(), p0 = ESP.getFreePsram();
    Mp3Stream* probe = new Mp3Stream();
    const bool ok = probe && probe->begin();
    const uint32_t i1 = ESP.getFreeHeap(), b1 = ESP.getMaxAllocHeap(), p1 = ESP.getFreePsram();
    if (probe) {
      delete probe;
    }
    const uint32_t i2 = ESP.getFreeHeap();
    log_e("MP3probe ok=%d internal %u->%u (cost %d) largest %u->%u psram %u->%u (cost %d) after_free %u",
          (int)ok, i0, i1, (int)i0 - (int)i1, b0, b1, p0, p1, (int)p0 - (int)p1, i2);
  }
#endif

  /* ── XFER_AUTOSTART ───────────────────────────────────────────────────────────────
   * Brings the music uploader up at boot so a file can be pushed to the card with no key
   * presses. Bench use only, and deliberately NOT a shipping feature: a server that opens
   * itself every boot is an open write endpoint on whatever network the phone joins.
   *
   * Build with -DXFER_AUTOSTART, then from a computer on the same WiFi:
   *   curl -F "rom=@track.mp3" http://wiphone.local/upload
   * Then rebuild without it. */
#ifdef XFER_AUTOSTART
  {
    static const XferConfig AUTO_CFG = {
      MUSIC_DIR, "Add music", ".mp3,.wav", "tracks", "download.mp3", "WiPhone-Music"
    };
    /* ⚠ WAIT for the station to associate first. xferStart() falls back to bringing up
     * its own access point when WiFi.status() != WL_CONNECTED, and at the end of setup()
     * the association is still in progress — so starting it here without waiting TEARS
     * DOWN the WiFi connection and puts the phone on its own AP instead. */
    for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
      delay(250);
    }
    log_e("XFER_AUTOSTART: wifi=%d ip=%s", (int)WiFi.status(),
          WiFi.localIP().toString().c_str());
    xferStart(&AUTO_CFG);
    log_e("XFER_AUTOSTART: uploader on at %s ap=%d", xferAddr(), (int)xferUsingAP());
  }
#endif


  /* ── MUSIC_SELFTEST ───────────────────────────────────────────────────────────────
   * Times the MP3 decoder on a real file from the card. Bench build only.
   *
   * This answers the one question the design could not answer from the desktop: helix's
   * state is in PSRAM, which is slower than internal RAM, and Audio::loop() decodes from
   * the MAIN LOOP alongside the screen and WiFi. A frame is 1152 samples = 26.1 ms of
   * audio at 44.1 kHz, so decode must stay well under that or playback stutters.
   *
   * Measures decode alone, with no I2S and no UI, which is the ceiling — whatever
   * headroom shows here is the most there will ever be. */
#ifdef MUSIC_SELFTEST
  {
    musicPlayerBegin();
    const int n = musicPlayerCount();
    log_e("SELFTEST: %d track(s) on the card", n);
    const MusicTrack* t = NULL;
    for (int i = 0; i < n; i++) {
      const MusicTrack* c = musicPlayerTrack(i);
      if (c && c->fmt == MUSIC_FMT_MP3) {
        t = c;
        break;
      }
    }
    if (t) {
      log_e("SELFTEST: %s  (%s)", t->name, t->path);
      File f = SD.open(t->path);
      if (f) {
        Mp3Stream* ms = new Mp3Stream();
        if (ms && ms->begin()) {
          uint8_t hdr[16];
          f.read(hdr, sizeof(hdr));
          uint32_t skip = mp3Id3v2Size(hdr, sizeof(hdr));
          f.seek(skip);
          log_e("SELFTEST: id3 tag %u bytes", skip);

          int16_t* pcm = (int16_t*)ps_malloc(MP3_MAX_FRAME_SAMPLES * sizeof(int16_t));
          Mp3Info info;
          memset(&info, 0, sizeof(info));
          uint32_t frames = 0, bad = 0, usDecode = 0, usRead = 0;
          const uint32_t t0 = millis();
          while (frames < 400 && (millis() - t0) < 20000 && pcm) {
            uint8_t tmp[512];
            size_t room = ms->space();
            if (room > sizeof(tmp)) {
              room = sizeof(tmp);
            }
            if (room > 0 && f.available()) {
              uint32_t r0 = micros();
              int got = f.read(tmp, room);
              usRead += micros() - r0;
              if (got > 0) {
                ms->fill(tmp, (size_t)got);
              }
            }
            uint32_t d0 = micros();
            int samples = ms->decode(pcm, &info);
            usDecode += micros() - d0;
            if (samples > 0) {
              frames++;
            } else if (samples < 0) {
              bad++;
            } else if (!f.available()) {
              break;
            }
          }
          const uint32_t wall = millis() - t0;
          if (frames > 0) {
            const uint32_t perFrame = usDecode / frames;
            const uint32_t budget = (uint32_t)(1152000000ULL / (info.sampleRate ? info.sampleRate : 44100));
            log_e("SELFTEST: %u frames %u bad, %d Hz %d ch %d kbps",
                  frames, bad, info.sampleRate, info.channels, info.bitrate / 1000);
            log_e("SELFTEST: decode %u us/frame, sd read %u us/frame, budget %u us -> %u%% of realtime",
                  perFrame, usRead / frames, budget,
                  (uint32_t)((uint64_t)(perFrame + usRead / frames) * 100 / budget));
            log_e("SELFTEST: %u frames in %u ms wall = %u ms of audio",
                  frames, wall, (uint32_t)((uint64_t)frames * 1152 * 1000 / (info.sampleRate ? info.sampleRate : 44100)));
          } else {
            log_e("SELFTEST: NO FRAMES DECODED");
          }
          if (pcm) {
            free(pcm);
          }
        } else {
          log_e("SELFTEST: decoder would not allocate");
        }
        if (ms) {
          delete ms;
        }
        f.close();
      } else {
        log_e("SELFTEST: could not open %s", t->path);
      }
    }
    log_e("SELFTEST: internal heap free %u largest %u psram %u",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
  }
#endif

  gui.state.booted = true;

  log_d("# # # # # # # # # # # # # # # # # # # # # # # # # # # #  END OF SETUP  # # # # # # # # # # # # # # # # # # # # # # # # # # # # ");
}

// # # # # # # # # # # # # # # # # # # # # # # # # # # # #  MAIN LOOP  # # # # # # # # # # # # # # # # # # # # # # # # # # # #

TinySIP sip;
char lastKeys[7];         // TODO: use RingBuffer?
uint32_t msLastKeyPress = 0;        // for any button being pressed
uint32_t msLastKeyInput = 0;        // for the keyboard timeouts during alphanumeric intputs
uint32_t msPowerOffStarted = 0;
uint32_t msHangingUp = 0;
uint32_t msHungUp = 0;
uint32_t msLastRtpPacket = 0;
uint32_t msLastBatt = 0;
uint32_t msLastUsbCheck = 0;
uint32_t msLastMinute = 0;
uint32_t msLastWifiRetry = -WIFI_RETRY_PERIOD_MS;
uint32_t msLastWiFiRssi = -WIFI_CHECK_PERIOD_MS;
uint8_t  rtpSilentCnt = 0x0;  // for the other praty rtp stream silent detection
bool keypadLedsOn = false;
bool updateMessageTimes = false;
bool waitingForClockUpdate = true;
uint8_t wifiTerminateSip = 0x0;

int8_t restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol;

uint32_t usbConnected = 0;        // DEBUG
uint32_t usbConnectedChecks = 0;  // DEBUG

bool lastTurnOff = true;

//uint32_t msProfileStart = 0;
//LinearArray<uint32_t, false> msProfile;

uint32_t last_lora_send = 0;

// Meshtastic new-message popup banner
#define MESH_POPUP_MS 2500u
static bool     meshPopupActive = false;
static uint32_t meshPopupShownMs = 0;

/* HOLD the two top corner keys — Select (top left) + Back (top right) — for two seconds to
 * sleep the screen.
 *
 * This replaces triple-tapping Back, which was tried twice and failed on hardware both times.
 * A repeated tap on a key that ALSO means "go back" is a bad gesture on this phone: the gap
 * has to stay tight or backing out of three nested screens sleeps the phone by accident, and
 * tight means the gesture itself is unreliable — measured at about six presses to register
 * three. It also depended on a "is the user typing?" test that could be poisoned by a stale
 * widget pointer and kill the feature for an entire power-on.
 *
 * A two-key chord has none of that. It cannot be produced by ordinary navigation, so the hold
 * can be long and forgiving; there is no window to miss; and it needs no guess about whether
 * a text field is being edited, because Back alone never triggers it. Nick's suggestion, and
 * the right one.
 *
 * Built on uiKeyDown, the held-key bitmask the keypad service already maintains (the SN7326
 * re-reports held keys about every 40 ms, and a stale-key sweep clears anything silent for
 * 350 ms), so this needs no new input plumbing. */
#define SLEEP_CHORD_MS   2000u
#define SLEEP_CHORD_MASK (WIPHONE_KEY_MASK_BACK | WIPHONE_KEY_MASK_SELECT)
/* Is a CALL actually happening?
 *
 * ⚠ Listed positively, and that matters. Both callers used to ask "is the state something
 * other than Idle/NotInited/HungUp", which sounds equivalent and is not: with no SIP proxy
 * reachable the phone rests in CallState::Error (12) FOREVER, so "not idle" was
 * permanently true. That silently broke two things — the CPU never dropped out of 240 MHz,
 * and the music-pauses-for-a-call edge had already fired at boot so it could never fire
 * for a real call. Error, Decline and HungUp are not calls; these eight are. */
/* ── THE BLACK BOX ──────────────────────────────────────────────────────────────────
 * Appends a line to /health.log on the card.
 *
 * Serial is no good for the two questions actually being asked. Measuring battery drain
 * needs the phone UNPLUGGED — plugged in it just charges — and an unplanned restart
 * happens when nobody is watching a terminal. So the log has to outlive both the cable
 * and the reboot, which means the card.
 *
 * Read it back with the uploader screen open: http://wiphone.local/log
 *
 * ⚠ Capped and wrapped rather than left to grow. A line a minute is ~90 KB a month, which
 * is nothing on the card, but a file that grows without limit is a slow leak of the one
 * resource the music library also needs. At the cap it starts again with a marker, so the
 * most recent hours always survive. */
#define HEALTH_LOG_PATH "/health.log"
#define HEALTH_LOG_TMP  "/health.tmp"
/* ⚠ RAISED 2026-08-23, 96K/32K -> 256K/128K, because 4 hours was not enough history to
 * answer the question this log exists for. Nick spent a day out with the phone on battery —
 * the first real on-battery discharge run there has ever been — and at the old settings the
 * retained window was ~250 lines, about FOUR HOURS. A drain run plus a car charge plus the
 * drive home does not fit in four hours, so the trim would have eaten the beginning of the
 * very run being measured, on the next boot, silently.
 *
 * The cost is nothing. A line is ~130 bytes and one is written per minute: 256 KB is about
 * 33 hours, ~2 MB a month at the cap, on a card that holds gigabytes and whose other
 * occupant is a 5 MB book. The original 96 K was cautious about a resource that turned out
 * not to be scarce, and the caution cost data twice — once on 2026-08-15 (the reset_reason
 * line for a restart, wrapped 29 minutes later) and nearly again today. */
#define HEALTH_LOG_MAX  (256 * 1024)
#define HEALTH_LOG_KEEP (128 * 1024)  // ~1000 lines, ~16 hours, retained across a trim

/* ── KEEP THE NEWEST HISTORY, NOT NONE OF IT ──────────────────────────────────────────
 * This used to hit the cap and SD.remove() the whole file. That threw away the most
 * recent hours at the exact moment they mattered: on 2026-08-15 the phone rebooted while
 * Nick was out, the log wrapped 29 minutes later, and the BOOT reset_reason= line for
 * that reboot — the one number the whole restart investigation turns on — was destroyed
 * before anyone could read it.
 *
 * Now it copies the last HEALTH_LOG_KEEP bytes forward and swaps the file in, so there
 * is always several hours of history no matter when you get to it.
 *
 * ⚠ Streamed in a 512-byte stack buffer, never loaded whole: internal heap is this
 * phone's scarcest resource and a 32 KB allocation here would be self-defeating.
 * ⚠ Runs roughly twice a day (64 KB of growth at ~130 bytes a line), so the one-off cost
 * of copying 32 KB is not worth optimising further.
 * Returns false if anything went wrong, and the caller then falls back to deleting —
 * an unbounded log is worse than a lost one. */
static bool healthLogTrim() {
  File in = SD.open(HEALTH_LOG_PATH, FILE_READ);
  if (!in) {
    return false;
  }
  const size_t total = in.size();
  if (total <= HEALTH_LOG_KEEP) {
    in.close();
    return true;                    // nothing to do
  }
  in.seek(total - HEALTH_LOG_KEEP);
  while (in.available() && in.read() != '\n') {
    ;                               // step over the partial line we landed in
  }

  SD.remove(HEALTH_LOG_TMP);        // a temp left by an interrupted trim
  File out = SD.open(HEALTH_LOG_TMP, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }
  out.println("--- trimmed: older entries dropped, newest kept ---");
  uint8_t buf[512];
  bool ok = true;
  while (in.available()) {
    const size_t n = in.read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    if (out.write(buf, n) != n) {
      ok = false;                   // card full or write error: keep the original
      break;
    }
  }
  in.close();
  out.close();
  if (!ok) {
    SD.remove(HEALTH_LOG_TMP);
    return false;
  }
  SD.remove(HEALTH_LOG_PATH);
  return SD.rename(HEALTH_LOG_TMP, HEALTH_LOG_PATH);
}

/* ── READ THE BLACK BOX WITHOUT ENDANGERING IT ─────────────────────────────────────────
 * Streams /health.log to the serial console. `health` on the console; see serial_cmd.cpp.
 *
 * ⚠ WHY THIS EXISTS WHEN http://wiphone.local/log ALREADY SERVES THE SAME FILE: reading it
 * over HTTP needs the WiFi uploader up, and that is precisely what this phone cannot afford
 * when contiguous internal heap is low. Measured 2026-08-23 after a 4.7 h run: free=11448,
 * largest=10560 — BELOW the ~16 KB baseline at which an 11 KB allocation has already
 * aborted the WiFi PHY and rebooted the phone once. The log exists to explain restarts, so
 * fetching it must never be the thing that causes one. It is also why the old note says
 * "one request, then close the screen, never poll".
 *
 * UART allocates nothing: a 256-byte stack buffer straight into the driver. No WiFi, no
 * sockets, no heap, and it works with the radio off and the card the only thing spinning.
 *
 * lastBytes = 0 dumps the whole file; otherwise the tail, rounded UP to a line boundary so
 * the first line is never a fragment that would parse as a bad sample.
 * Returns the file's total size, or -1 if there is no log to read. */
int healthDump(uint32_t lastBytes) {
  if (!gui.state.cardPresent) {
    return -1;
  }
  File f = SD.open(HEALTH_LOG_PATH, FILE_READ);
  if (!f) {
    return -1;
  }
  const uint32_t total = f.size();
  if (lastBytes && total > lastBytes) {
    f.seek(total - lastBytes);
    while (f.available() && f.read() != '\n') { }    // discard the partial first line
  }
  char buf[256];
  while (f.available()) {
    const int n = f.readBytes(buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    uart_write_bytes(UART_NUM_0, buf, (size_t)n);
  }
  f.close();
  /* The trailer is printed HERE, by the file that owns HEALTH_LOG_MAX/KEEP, because the
   * caller cannot know them without restating them — and a restated copy in serial_cmd.cpp
   * went stale within the hour the cap was raised. The size matters as much as the
   * contents: a file near the cap means the next trim is imminent and older samples are
   * about to go, which is exactly how a discharge run gets half-eaten before it is read. */
  const int wrote = snprintf(buf, sizeof(buf),
                             "\n--- end · %u bytes · cap %u · trims to newest %u ---\n",
                             (unsigned)total, (unsigned)HEALTH_LOG_MAX, (unsigned)HEALTH_LOG_KEEP);
  if (wrote > 0) {
    uart_write_bytes(UART_NUM_0, buf, (size_t)wrote);
  }
  return (int)total;
}

/* Not static: GUI.cpp's app-open heap probe writes here too, so the culprit lands in the
 * same durable log everything else is read from. */
void healthLogLine(const char* line) {
  if (!gui.state.cardPresent) {
    return;
  }
  File f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
  if (!f) {
    return;
  }

  /* ⚠ Trim BEFORE writing the boot line, not after. The size check used to sit below it,
   * so a boot line could be written and then immediately destroyed by the very same call
   * that wrote it — losing the reset reason on exactly the boot that followed a long run. */
  if (f.size() > HEALTH_LOG_MAX) {
    f.close();
    if (!healthLogTrim()) {
      SD.remove(HEALTH_LOG_PATH);   // last resort; never let it grow without bound
    }
    f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
    if (!f) {
      return;
    }
  }

  if (bootLine[0]) {
    f.println("");
    f.println(bootLine);      // why the last run ended, written once the card is up
    bootLine[0] = '\0';
  }
  f.println(line);
  f.close();
}

/* ── "DOES ANYTHING NEED 240 MHz" IS NOT THE SAME QUESTION AS "IS A CALL UP" ──────────
 * MEASURED 2026-08-15, from the car log: the phone sat at `sip=6` (CallState::HangUp) with
 * the screen off and no network (`wifi=1`) for 19+ consecutive minutes, and the CPU stayed
 * at 240 MHz the whole time because sipCallActive() counts HangUp as a call. At 80 MHz the
 * core draws roughly half as much, so this was about double the idle current — while out of
 * range in a car, which is exactly when the battery matters most.
 *
 * HangUp and HangingUp are TEARDOWN states. HangUp's own comment says it "can be triggered
 * from any other state", and HangingUp is "waiting for confirmation of BYE/CANCEL,
 * resending" — with no proxy reachable that confirmation never comes and the phone rests
 * there indefinitely. Neither has an audio session to protect.
 *
 * ⚠ This is the SAME TRAP the repo already hit once and wrote up: the phone resting forever
 * in a state that the "is a call happening" test counts as active. That fix replaced an
 * exclusion test with a positive list of eight states — correct as far as it went, but two
 * of those eight can also stick.
 *
 * ⚠ DELIBERATELY A SEPARATE PREDICATE, not an edit to sipCallActive(). That one ALSO gates
 * music-pauses-for-a-call, and quietly changing a shared predicate to fix an unrelated
 * caller is the exact bug class that produced three separate faults in this codebase today.
 * The CPU gate asks its own question. */
/* May SIP poll the network this pass?
 *
 * ⚠ A NEW predicate on purpose. sipNeedsFullSpeed() and sipCallActive() already exist and mean
 * different things, and this codebase has been bitten repeatedly by one caller quietly editing
 * a shared predicate another caller depended on. Every consumer states what it needs.
 *
 * WHY: the Game Boy commits 16 KB of INTERNAL RAM that cannot be relocated — an 8,192-byte
 * blit task stack, a 4,096-byte emulator task stack and a 4,096-byte audio buffer. FreeRTOS
 * stacks physically cannot run from PSRAM on the ESP32, so unlike the ROM and both
 * framebuffers (already correctly in PSRAM) none of this can move. Meanwhile every SIP poll
 * calls WiFiUDP::parsePacket(), which wants 1,460 CONTIGUOUS bytes from that same pool. With
 * ~26 KB free, the emulator takes 16 and the two of them fight over what is left.
 *
 * So while the emulator is on screen, SIP stops polling entirely: no UDP reads, no re-REGISTER.
 * Registration lapses after its 60 s expiry and re-establishes when the game exits. The cost is
 * missed calls during a game — the trade the phone's owner explicitly asked for.
 *
 * ⚠ A LIVE CALL OUTRANKS THE EMULATOR. If audio is already flowing, dropping it because the
 * games menu got opened would be worse than the memory pressure it saves. */
static bool sipMayPoll() {
  switch (gui.state.sipState) {
    case CallState::InvitingCallee:
    case CallState::InvitedCallee:
    case CallState::RemoteRinging:
    case CallState::Call:
    case CallState::BeingInvited:
    case CallState::Accept:
    case CallState::HangUp:
    case CallState::HangingUp:
      return true;                 // mid-call or mid-teardown: keep servicing it
    default:
      break;
  }
  return !gui.isAppRunning(GUI_APP_GBC);
}

static bool sipNeedsFullSpeed() {
  switch (gui.state.sipState) {
    case CallState::InvitingCallee:
    case CallState::InvitedCallee:
    case CallState::RemoteRinging:
    case CallState::Call:
    case CallState::BeingInvited:
    case CallState::Accept:
      return true;                 // a live or imminent audio session
    default:
      return false;                // includes HangUp/HangingUp: teardown, and they stick
  }
}

static bool sipCallActive() {
  switch (gui.state.sipState) {
    case CallState::InvitingCallee:
    case CallState::InvitedCallee:
    case CallState::RemoteRinging:
    case CallState::Call:
    case CallState::HangUp:
    case CallState::HangingUp:
    case CallState::BeingInvited:
    case CallState::Accept:
      return true;
    default:
      return false;
  }
}

#define MUSIC_F2_HOLD_MS 500           // past this, F2 means "previous" rather than "next"
static uint32_t msF2Down = 0;          // when F2 went down, 0 = not held
static bool     f2Fired = false;       // the hold already acted; ignore the release
static uint32_t msChordStart = 0;      // when both corners went down, 0 = not held
static bool     chordFired = false;    // one sleep per hold, not one every loop

// Quiet "pop" sound on a new Meshtastic message (one-shot). The PCM player
// loops, so we stop it by timer after it has played through once.
#define MESH_POP_MS 280u
static bool     meshPopPlaying = false;
static uint32_t meshPopStartMs = 0;

// Short silent vibration on a new Meshtastic message.
#define MESH_VIBRO_MS 180u
static bool     meshVibroActive = false;
static uint32_t meshVibroStartMs = 0;

/* The brief "you have a message" pop + buzz.
 *
 * ⚠ SHARED BY THE MESH AND SIP PATHS, and that is the whole point of it existing.
 * An incoming SIP TEXT never announced itself — only mesh messages did. That went unnoticed
 * while texts were reaching the phone through the LoRa mirror, because those arrive on the
 * MESH path and chirped as a side effect. Silencing the mirror (which is right: a mirrored
 * copy of a text you already have is not news) therefore took the notification away from
 * real incoming texts too, and Nick noticed immediately: "I seem to have lost the vibration
 * for notifications, and the chirp."
 *
 * So the announcement now belongs to the ARRIVAL, not to the transport that carried it.
 * Both callers share this, and the teardown timers below already service either one. */
static void notifyMessageArrived(uint32_t now, uint8_t mode);

/* The mirror's way in. Non-static so meshtastic_service.cpp and the LAN poller can reach it
 * without either of them needing to know about ringer modes or the vibro motor. */
void smsMirrorNotifyArrival() {
  notifyMessageArrived(millis(), gui.state.notifySipMode);
}

static void notifyMessageArrived(uint32_t now, uint8_t mode) {
  /* `mode` is this KIND of arrival's own setting from Settings > Notifications — a text and
   * a mesh message are separately configurable, because muting one used to mute the other.
   *
   * ⚠ ONE log_e SURVIVES, DELIBERATELY. "It did not vibrate" was reported three times and
   * guessed at three times; the line that settled it was this path saying what it chose and
   * why. It fires once per arrival, which is rare enough to be worth keeping — and `mode`
   * is the answer to the most common report, since VIBRATE_ONLY looks exactly like a broken
   * chirp. */
  if (mode == ControlState::RINGER_SILENT) {
    log_e("NOTIFY: silent (mode=%d) - badge only", (int)mode);
    return;
  }
  const bool callBusy = (gui.state.sipState == CallState::Call);
  /* THE MOTOR GOES FIRST, before the pop — deliberately reordered. The buzz is a plain
   * GPIO write through the I2C extender; the pop reconfigures and drives the audio codec.
   * If the pop's I2C traffic can ever disturb the extender (the standing suspicion behind
   * "it chirped but never buzzed"), buzzing first removes that whole failure class instead
   * of measuring it. Costs nothing: the pulse runs ~180 ms and the pop starts within a
   * millisecond of it either way.
   *
   * "Vibrate only" still buzzes — that is the whole difference from Silent.
   *
   * ⚠ A SECOND MESSAGE RE-ARMS THE TIMER; IT DOES NOT GET SKIPPED. Guarding this on
   * `!meshVibroActive` silently ate any notification that landed inside the previous 180 ms
   * pulse, which is exactly what Nick saw: "only heard one buzz out of 3 sent texts", and
   * the log agreed — buzz, SKIPPED, buzz. Restarting the pulse is also what the original
   * code did before the guard was added, and it degrades the right way: a burst of texts
   * becomes one longer buzz rather than one short buzz and silence. */
  if (!gui.state.ringing) {
    allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
    meshVibroActive = true;
    meshVibroStartMs = now;              // re-arm, so back-to-back arrivals keep it buzzing
    log_e("NOTIFY: buzz %u ms (mode=%d)", (unsigned)MESH_VIBRO_MS, (int)mode);
  } else {
    /* ⚠ SAY SO. The one log line proving this path ran used to sit INSIDE the gate, so a
     * `ringing` flag left latched by any teardown path would kill every later buzz with no
     * trace at all — indistinguishable from the motor being broken. If this line ever shows
     * while the phone is plainly not ringing, the latched flag is the fault to chase. */
    log_e("NOTIFY: suppressed (ringing latched)");
  }
  if (mode == ControlState::RINGER_RING_AND_VIBRATE) {
    /* The pop is genuinely one-at-a-time — it is a PCM file playing through the codec, and
     * restarting it mid-play is how the audio device gets left in the wrong mode (see
     * Audio::preserve()). A burst therefore gets one sound and one long buzz, which is the
     * right shape: you do not want five chirps, but you do want to feel every arrival. */
    if (!(callBusy || gui.state.ringing || meshPopPlaying)) {
      const bool played = audio->playPop(&SPIFFS, gui.state.notifyVolume);
      if (played) {
        meshPopPlaying = true;
        meshPopStartMs = now;
      }
    }
  }
}


extern void gbcXferHandleClient();   // ROM-transfer web server pump (no-op when off)
extern bool gbcXferOn();             // true while the transfer server is running

/* ── RAISE THE CLOCK BEFORE THE WORK, NEVER AFTER ─────────────────────────────────
 * The frequency gate lives at the BOTTOM of loop(), which is correct for deciding to
 * come DOWN but useless for going UP: a keypress is dispatched and the screen repainted
 * hundreds of lines earlier, so the one frame a person actually perceives would render
 * at the low clock and the boost would arrive after it was needed. Moving the gate is
 * NOT the fix - it would invert the wake path, which deliberately repaints before the
 * gate is reached.
 *
 * So the gate keeps its place and its down-path, and this raise-only helper runs at the
 * moment a key is dequeued. It shares gCpuCurMhz with the gate, so the two can never
 * disagree about what the hardware is doing. Raising only: nothing here can ever put the
 * phone into a lower state than the gate chose. */
static uint32_t gCpuCurMhz = 240;

/* ⚠ IF THIS IS EVER RE-ENABLED, READ THE DEADLOCK NOTE ON THE `busy` PREDICATE FIRST.
 * A RAISE is a frequency change like any other, so it fires uart_on_apb_change() and can park
 * the loop task forever while the plate's GPS is streaming at 115200. It is safe today only
 * because gGpsNmea is in `busy`, which pins the clock at 240 so the `!= 240` test below is
 * false and this never calls. That is load-bearing, not incidental. */
__attribute__((unused)) static void cpuRaiseForUi() {
  if (gCpuCurMhz != 240) {
    setCpuFrequencyMhz(240);
    gCpuCurMhz = 240;
  }
}

void loop() {
  while (1) {
    esp_task_wdt_reset();          // DIAGNOSTIC: see setup(). loop() never returns, so the
                                   // Arduino core's own feed never runs — this is the feed.
    uint32_t now = millis();
#ifdef USER_SERIAL
    /* The deferred GPS retune. First pass only, and deliberately HERE rather than at the
     * end of setup(): this is the exact path that has always survived — a live 115200
     * line meeting updateBaudRate() rather than esp_intr_alloc(). See setup(). */
    if (gGpsBaudPending) {
      gGpsBaudPending = false;
      gpsApplyBaud(true);
      gGpsReader.reset();        // the boot's worth of 9600-misframed bytes is not data
      log_e("GPS: NMEA reader ON (user UART %d/%d @ %u) - retuned from loop()",
            USER_SERIAL_RX, USER_SERIAL_TX, (unsigned)gGpsBaud);
    }
#endif
    /* ── SUPERLOOP STALL DETECTOR ──────────────────────────────────────────────────────────
     * Nick, 2026-08-24: "sometimes when scrolling menus, the phone will freeze for a second
     * or two, wifi will drop, then it will unfreeze and WiFi comes back up." Everything in
     * this firmware shares ONE task, so anything that blocks here blocks the keypad, the
     * screen AND the WiFi stack together — which is exactly that symptom, and exactly what
     * the sms-mirror comment below already warns about ("not slow, it is the 5-second freeze
     * bug rebuilt on purpose").
     *
     * A freeze that rare cannot be caught by watching; it has to be caught by the phone. This
     * measures the gap between successive passes — i.e. how long the PREVIOUS pass took — and
     * names it at the compiled-in level so it lands in the serial log and can be correlated
     * with whatever else was happening. Costs one millis() compare per pass.
     * 250 ms is well above any normal pass (the idle tick is 5 ms) and well below the "second
     * or two" being chased, so it should be silent until something real happens. */
    {
      static uint32_t sLastPassMs = 0;
      if (sLastPassMs) {
        const uint32_t took = now - sLastPassMs;
        if (took > 250) {
          log_e("LOOP STALL: %u ms in one pass (scr=%d cpu=%uMHz wifi=%d) - WiFi/keypad/screen "
                "were all frozen for this long", (unsigned)took, (int)gui.state.screenBrightness,
                (unsigned)(getCpuFrequencyMhz()), (int)WiFi.status());
          /* ⚠ AND INTO /health.log, because serial only helps when somebody is watching it.
           * The freeze this exists to catch happens while Nick is USING the phone — walking
           * around, off the cable — and a diagnostic that only fires for a tethered developer
           * would miss every real occurrence. Same argument as the health line itself.
           * Rate-limited to one record a minute: a phone that stalls repeatedly must not spend
           * its stalls writing about stalling. */
          static uint32_t sLastStallLogMs = 0;
          if (!sLastStallLogMs || (uint32_t)(now - sLastStallLogMs) > 60000) {
            sLastStallLogMs = now;
            char stallLine[96];
            snprintf(stallLine, sizeof(stallLine), "STALL %ums scr=%d cpu=%u wifi=%d",
                     (unsigned)took, (int)gui.state.screenBrightness,
                     (unsigned)(getCpuFrequencyMhz()), (int)WiFi.status());
            healthLogLine(stallLine);
          }
        }
      }
      sLastPassMs = now;
    }
    /* The mesh database save blocks this task for ~1.5 s, so it is held off while the phone is
     * being used — see MeshtasticService::setUiIdle(). Three seconds after the last key, the
     * user has stopped scrolling and a freeze there costs nothing. This is the only place that
     * can see both the keypad and the mesh service. */
    meshService.setUiIdle(gLastKeyMs == 0 || (uint32_t)(now - gLastKeyMs) > 3000);
    meshService.setCardPresent(gui.state.cardPresent);   // the DB lives on SD when there is one
    gbcXferHandleClient();
    serialCmdLoop();       // USB console: `?` for help. Costs nothing when nothing is typed.

    /* Pull mirrored texts from COVEY over the LAN — one bounded step per pass, never a
     * blocking request. See sms_mirror_poll.h: the UI is one task, so a blocking GET here is
     * not "slow", it is the 5-second freeze bug rebuilt on purpose.
     *
     * ⚠ Called UNCONDITIONALLY, with sipMayPoll() passed IN. Wrapping the call in that gate
     * instead looked equivalent and was not: while the Games app is open the gate is false,
     * so the poller could not read a config file that had just been uploaded — and reported
     * nothing about it, because the reporting was inside the skipped call. The socket is the
     * only part that needs gating. */
    smsMirrorPollLoop(sipMayPoll());

    /* THE MIRROR'S ANNOUNCER — one place, both transports. smsMirrorIngestLine() latches
     * news whenever it stores a text (whether the line rode in over LoRa or the LAN poll
     * above), and this takes it. The split serves two different urgencies:
     *   - the BUZZ is immediate: feeling the arrival late is the same as not feeling it;
     *   - the UI EVENT is coalesced (~700 ms): each NEW_MESSAGE_EVENT makes an open
     *     Messages screen rebuild its snapshots — four 120-deep paged scans — so a
     *     catch-up burst of records must become a handful of rebuilds, not one per text.
     * Before this, the LAN path announced NOTHING (no buzz, no event — a text stored in
     * silence), and a mirrored text arriving while a thread was open stayed invisible
     * until you backed out and re-entered — at which point the sort dropped it mid-list,
     * muddying the ordering bug's report. */
    {
      static bool     s_mirrorUiPending = false;
      static uint32_t s_mirrorUiLastMs  = 0;
      bool inbound = false;
      if (smsMirrorTakeNews(&inbound)) {
        s_mirrorUiPending = true;
        if (inbound) {
          smsMirrorNotifyArrival();
        }
      }
      if (s_mirrorUiPending && elapsedMillis(now, s_mirrorUiLastMs, 700)) {
        s_mirrorUiLastMs = now;
        s_mirrorUiPending = false;
        appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
      }
    }

    /* 🔎 RATCHET WATCHPOINT — name the MOMENT `largest` steps down.
     *
     * `largest` is the number that predicts the abort; free and min disagree with it and
     * predict nothing. It steps down permanently rather than drifting, so catching the step
     * — and what the phone was doing at that instant — is the whole game. The HEALTH line
     * samples every 15 s, far too coarse: it says the block shrank somewhere in the last
     * quarter minute but never says during what.
     *
     * Polled at 250 ms, and prints ONLY on a real drop, so an idle phone logs nothing. The
     * high-water mark follows recoveries upward so the next genuine dip still registers.
     *
     * ⚠ heap_caps_get_largest_free_block() walks the free list, so this is not free. It is a
     * DIAGNOSTIC: delete it, or raise DROP_THRESH, once the ratchet is understood. This is the
     * same trap as the health log itself — an instrument that consumes what it measures. */
    {
      /* Downshifted 250 ms → 2 s and 512 → 1024 B now the ratchet is understood and 0.9.3
       * has soaked: the free-list walk four times a second was itself a measurable cost
       * (the handoff's own instruction was to delete it or raise the threshold once
       * trusted). At 2 s it still names the moment of any real step within the window the
       * HEALTH line could never resolve, for an eighth of the walks. */
      const uint32_t DROP_THRESH = 1024;
      static uint32_t s_lastLargestCheck = 0;
      static uint32_t s_largestHigh = 0;
      if (elapsedMillis(now, s_lastLargestCheck, 2000)) {
        s_lastLargestCheck = now;
        const uint32_t lg = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (s_largestHigh == 0 || lg > s_largestHigh) {
          s_largestHigh = lg;
        } else if (lg + DROP_THRESH < s_largestHigh) {
          log_e("DROP largest %u->%u (-%u) app=%d sip=%d wifi=%d scr=%d cpu=%luMHz up=%lus",
                (unsigned)s_largestHigh, (unsigned)lg, (unsigned)(s_largestHigh - lg),
                (int)gui.currentAppId(), (int)gui.state.sipState, (int)WiFi.status(),
                (int)gui.state.screenBrightness, (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)(now / 1000));
          s_largestHigh = lg;
        }
      }
    }

    /* Stale-held-key sweep, driven by the SN7326's ~40ms held-key re-reports
     * (LONGPRESS_DELAY(1)): a key silent for 350ms lost its release event.
     * If holds ever break rhythmically at ~350ms, the chip's re-report isn't periodic at
     * this setting — revert to DELAY(2) and 5200ms here.
     *
     * ⚠ BOTH MASKS, not just uiKeyDown. It used to clear keypadState in game mode only,
     * on the belief that keypadState was game-side state — but the UI press handler was
     * nested inside the keypadState edge, so a keypadState bit left set after a lost
     * release vetoed every subsequent press of that key. Half-sweeping was therefore
     * worse than not sweeping: it looked like the key had been rearmed when it had not.
     * The UI handler no longer consults keypadState, and this now clears both, so the two
     * masks cannot disagree about which keys are down. */
    /* ⚠ THE SWEEP'S PREMISE IS "WE WOULD HAVE SEEN A HEARTBEAT IF THERE HAD BEEN ONE",
     * and that is only true if we were LOOKING. This runs before the keypad drain in the
     * same pass, so after the loop blocks — an SD write, the uploader, a WiFi call — the
     * heartbeats of a genuinely held key are still sitting in the chip, unread, and
     * keyLastSeenMs is stale through no fault of the key. Sweeping on that evidence
     * releases a key the user is still holding, and the queued heartbeats then decode as
     * a fresh press: auto-repeat, the exact fault uiKeyDown was added to kill.
     *
     * So skip a pass that arrives late and let the drain below refresh the timestamps
     * first. One pass is enough — a single keyboardRead() empties the whole FIFO. */
    static uint32_t s_lastSweepMs = 0;
    const bool loopWasStalled = s_lastSweepMs && (now - s_lastSweepMs) > 100;
    s_lastSweepMs = now;
    for (uint32_t st = loopWasStalled ? 0 : (uiKeyDown | keypadState); st; st &= st - 1) {
      int b = __builtin_ctz(st);
      if (now - keyLastSeenMs[b] > 350) {
        /* Every trip through here is a RELEASE THAT WAS NEVER SEEN, and the key was deaf
         * for the whole 350ms getting here. This is the counter the keypad poll is meant
         * to drive to zero: with the poll working, releases are recovered within ~40ms
         * and the sweep should have almost nothing left to clean up. */
        if (uiKeyDown & (1u << b)) {
          kcStaleSwept++;
        }
        uiKeyDown   &= ~(1u << b);
        keypadState &= ~(1u << b);
        /* Deliberately NOT stamping keyLastUpMs here. The stall guard above is what keeps
         * a queued heartbeat from reading as a fresh press; using the bounce window for
         * that job instead would put a 40 ms hole right where the user, having had one
         * tap swallowed, is most likely to be tapping again. */
      }
    }

    /* Sleep the screen on Select + Back held together for two seconds. See the note by
     * SLEEP_CHORD_MS. Checked here, right after the stale-key sweep, so it reads a held-key
     * mask that is already up to date; nothing else in the loop has to know about it.
     *
     * Deliberately does NOT swallow the keys. Both are edge-triggered, so their presses were
     * dispatched the moment they went down and cannot be taken back — Back will have
     * navigated once. That is a fair price for a gesture that works every time, and the
     * screen going off is its own confirmation. */
    /* ── F2: next on a tap, previous on a hold ─────────────────────────────────────
     * Decided on RELEASE, because "short or long" is not knowable when the key goes
     * down — firing next immediately would make every hold skip forward first.
     *
     * Previous is musicPlayerPrev(), which does the thing every music player does:
     * within the first three seconds it goes to the previous track, after that it
     * restarts the current one.
     *
     * Read here, right after the stale-key sweep, so uiKeyDown is already up to date. */
    if (!gGbcActive && musicPlayerCurrent() >= 0) {
      if (uiKeyDown & WIPHONE_KEY_MASK_F2) {
        if (!msF2Down) {
          msF2Down = now;
          f2Fired = false;
        } else if (!f2Fired && now - msF2Down >= MUSIC_F2_HOLD_MS) {
          f2Fired = true;                  // one action per hold, however long it lasts
          musicPlayerPrev();
        }
      } else {
        if (msF2Down && !f2Fired) {
          musicPlayerNext();               // released before the threshold: a tap
        }
        msF2Down = 0;
        f2Fired = false;
      }
    } else {
      msF2Down = 0;
      f2Fired = false;
    }

    if ((uiKeyDown & SLEEP_CHORD_MASK) == SLEEP_CHORD_MASK) {
      if (!msChordStart) {
        msChordStart = now;
        chordFired = false;
      } else if (!chordFired && now - msChordStart >= SLEEP_CHORD_MS) {
        chordFired = true;               // one sleep per hold, however long it is held
        if (gui.state.screenBrightness > 0) {
          gui.sleepScreen();
        }
      }
    } else {
      msChordStart = 0;
      chordFired = false;
    }

    // DEBUG
    //uint32_t loopTime = micros();
    //if (!msProfileStart) msProfileStart = loopTime;

    appEventResult redrawWhat = DO_NOTHING;

    // Power button check
    if (gpioExtenderEvent) {
      if (gpioExtenderServiceInterrupt()) {
        if (powerButtonPressed) {
          msPowerOffStarted = now;
        } else if (poweringOff) {
          redrawWhat |= gui.processEvent(now, POWER_NOT_OFF_EVENT);
          poweringOff = false;
        }
      }
    }

    // Headphone check
    if (headphoneEvent) {
      headphoneServiceInterrupt();
    }

    // KEYPAD + TICK

    // Check if interrupt occured
    uint8_t toRead = keypadToRead;

    /* Drain the keypad FIFO periodically WITHOUT an interrupt as well. The chip's INT is
     * a 10ms PULSE that auto-clears whether or not anyone read it, so an event arriving
     * while INT is already low (or lost to an I2C error) sits in the FIFO with no edge
     * left to announce it — a missed tap, or a button stuck down until the next press.
     * See the long note by KEYPAD_POLL_MS for why one release in four is exposed to this.
     *
     * WHEN, not always: while a key is believed down — which is exactly when a release
     * may be stuck — and for a second after the last event. An idle phone does no I2C
     * here at all, so this costs nothing on the battery, which is the point.
     *
     * The game keeps its own unconditional 150ms cadence: it runs its own inner loop and
     * cares about held state rather than edges. */
    /* ⚠ THE UI POLL WAS TRIED AND IS RETIRED — MEASURED, NOT ASSUMED. It was added on the
     * theory that the chip's 10ms INT pulse was stranding events in the FIFO; the raw
     * trace refuted that outright, `drained` staying at 0 through whole sessions of menu
     * use while `empty` climbed into the hundreds. So it recovered nothing and spent I2C
     * traffic every 40ms to do it. Worse, it made things AMBIGUOUS: an empty FIFO and a
     * release both read back as 0x00, and only an interrupt proves an event really
     * happened — which is the discriminator the release fix now depends on.
     * The real fault was never transport; it was that the chip does not say WHICH key
     * came up. See the note by the code-0 release attribution above.
     * The game keeps its own 150ms drain: it reads held state rather than edges. */
    static uint32_t msLastKeyDrain = 0;
    bool polledRead = false;
    if (!toRead && gGbcActive && elapsedMillis(now, msLastKeyDrain, 150)) {
      toRead = 1;
      polledRead = true;
    }

    // Read all the keys to buffer
    if (toRead) {
      msLastKeyDrain = now;
      keyboardRead(polledRead);
    }
#ifdef USE_VIRTUAL_KEYBOARD
    keyboardUdpRead();
#endif

    // Process keys buffer
    EventType keyPressed;
    bool anyPressed = false;
    while (!keypadBuff.empty()) {
      // Retrieve button from buffer safely
      keyPressed = keypadBuff.get();
      if (keyPressed) {
        gLastKeyMs = millis();     // see setUiIdle() below: holds off the blocking DB save
      }
#if UI_IDLE_DOWNCLOCK
      if (keyPressed) {
        cpuRaiseForUi();     // ⚠ touches the input path — see UI_IDLE_DOWNCLOCK
      }
#endif


      // Process key
      Random.feed(rotate5(now) ^ keyPressed);

      // Shift old characters and remember current character
      for (uint8_t k = sizeof(lastKeys) - 1; k > 0; k--) {
        lastKeys[k] = lastKeys[k - 1];
      }
      lastKeys[0] = keyPressed;

      // Triple-tap the top-right (Back) button to sleep the screen. Only tracked
      // while the screen is awake, so a wake-up tap doesn't count.
      if (!anyPressed && gui.state.inputType == InputType::AlphaNum) {
        msLastKeyInput = now;
      }
      anyPressed = true;

      // Process key
#ifndef STEAL_THE_USER_BUTTONS
      /* ── The four user buttons are the music transport ──────────────────────────
       * F1 play/pause · F2 next (HELD = previous) · F3 louder · F4 quieter
       *
       * Handled here rather than in MusicApp so they work from any screen — which is
       * the point of playback that outlives its app. The keys are SWALLOWED when used
       * (keyPressed = 0), otherwise the screen underneath acts on them too.
       *
       * ⚠ Only while a track is loaded. Books uses F3/F4 to page and the Game Boy uses
       * F1/F2 for its own volume; taking them unconditionally would break both. With
       * nothing loaded these fall through exactly as before.
       *
       * ⚠ F2 is NOT handled here — it fires on RELEASE, next to the sleep chord, because
       * short-versus-held cannot be decided at the moment of the press. */
      /* Two different tests, because two different apps claim these keys.
       *
       * F1/F2 (play-pause, next-prev) are claimed by nothing else, so a LOADED track is
       * enough — that is what lets F1 restart a paused track from any screen.
       *
       * F3/F4 are Books' page keys, so volume only takes them while music is actually
       * PLAYING. Read a book with music paused and paging works exactly as it always
       * did; read one with music playing and the side buttons are volume, which is what
       * you want when there is sound coming out. */
      const bool musicLoaded  = !gGbcActive && musicPlayerCurrent() >= 0;
      const bool musicSounding = musicLoaded && musicPlayerIsPlaying();

      if (musicLoaded && keyPressed == WIPHONE_KEY_F1) {
        musicPlayerTogglePause();
        keyPressed = 0;
      }

      if (musicSounding && keyPressed == WIPHONE_KEY_F3) {
        musicPlayerVolumeStep(+1);
        keyPressed = 0;
      }

      if (musicSounding && keyPressed == WIPHONE_KEY_F4) {
        musicPlayerVolumeStep(-1);
        keyPressed = 0;
      }

      if (musicLoaded && keyPressed == WIPHONE_KEY_F2) {
        keyPressed = 0;      // consumed; the action fires on release or on the hold timer
      }

      if (keyPressed == WIPHONE_KEY_END && !gGbcActive) {
        // During a Game Boy session END is the pause-menu button; don't also
        // poke the SIP state machine (it popped call UI over the game / froze).
        gui.state.setSipState(CallState::HangUp);
      }




#else
      if (keyPressed == WIPHONE_KEY_F1) {
        gui.toggleScreen();
        //allDigitalWrite(ENABLE_DAUGHTER_33V, HIGH);
        //gui.longBatteryAnimation();
        allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
        allDigitalWrite(KEYBOARD_LED, LOW);
        delay(500);
        allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        allDigitalWrite(KEYBOARD_LED, HIGH);
        gui.toggleScreen();
      }

      if (keyPressed == WIPHONE_KEY_F2) {
        audio_test();
      }

      if (keyPressed == WIPHONE_KEY_F3) {
        bool isLoud = !audio->isLoudspeaker();
        log_d("Loudspeaker: %d", isLoud);
        audio->chooseSpeaker(isLoud);
        log_d("WiFi Mode: %d", WiFi.getMode());
        log_d("WiFi Status: %d", WiFi.status());
      }

      if (keyPressed == WIPHONE_KEY_F4) {
        //esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN,1);
        //esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN,0); //1 = Low to High, 0 = High to Low. Pin pulled HIGH
        //esp_sleep_enable_timer_wakeup(5000000);
        log_d("begin delay");
        delay(500); // debounce
        log_d("begin light sleep");
        delay(10); // let print finish
        //esp_deep_sleep_start();
        esp_light_sleep_start();
        log_d("awake!!!");

        //LOG_MEM_STATUS;

        //gui.setDumpRegion();
        //gui.frameToSerial();    // "screenshot"
      }
#endif

      // Process event in GUI
      redrawWhat |= gui.processEvent(now, keyPressed);
      //log_d("redrawWhat = 0x%x", redrawWhat);

      // Check for "Easter eggs"
      // Easter eggs are currently broken, probably because we now use numeric key entry to dial numbers by default, and can't type '*'
      // (this is not the only thing preventing it from working, but after checking the lack of '*' I stopped)
      if (!memcmp(lastKeys, "##", 2)) {
        // check for Easter eggs
        if (!memcmp(lastKeys + 2, "101**", 5)) {    // **101##   +
          log_d("Easter egg = 101: starting an SIP client");
          gui.state.setSipState(CallState::InvitingCallee);
        } else if (!memcmp(lastKeys + 2, "301**", 5)) {    // **103##
          log_d("Easter egg = 103: send register request");
          sip.registration();
        } else if (!memcmp(lastKeys + 2, "601**", 5)) {    // **106##
          //log_d("Easter egg = 106: send message test");
          log_d("Easter egg = 106: send message test. Add a sip account in WiPhone.ino to use this test");
          sip.sendMessage("sip:user@host.com", "Hello from WiPhone");
        } else if (!memcmp(lastKeys + 2, "701", 5)) {    // **107##
          log_d("Easter egg = 107: test motor and blink LED");
          allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
          allDigitalWrite(KEYBOARD_LED, LOW);
          delay(2500);
          allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
          allDigitalWrite(KEYBOARD_LED, HIGH);
        } else if (!memcmp(lastKeys + 2, "801**", 5)) {    // **108## - soft off
          log_d("Easter egg = 108: soft off");
          allDigitalWrite(POWER_CONTROL, HIGH);

          // AUDIO

        } else if (!memcmp(lastKeys + 2, "002**", 5)) {    // **200##
          log_d("Easter egg = 200: audio test on");
          audio_test();
        } else if (!memcmp(lastKeys + 2, "102**", 5)) {    // **201##
          log_d("Easter egg = 201: audio shutdown (audio test off)");
          audio->shutdown();
        } else if (!memcmp(lastKeys + 2, "202**", 5)) {    // **202##
          log_d("Easter egg = 202: sending RTP stream from microphone");
          audio->openRtpConnection(5000);
          audio->sendRtpStreamFromMic(Audio::G722_RTP_PAYLOAD, IPAddress(192, 168, 1, 15), 5000);
        } else if (!memcmp(lastKeys + 2, "302**", 5)) {    // **203##   - (receiving from distant server)
          log_d("Easter egg = 203: play incoming RTP stream");
          audio->openRtpConnection(5000);
          audio->playRtpStream(Audio::G722_RTP_PAYLOAD);
        } else if (!memcmp(lastKeys + 2, "402**", 5)) {    // **204##
          log_d("Easter egg = 204: recording mic audio");
          audio->setBitsPerSample(16);
          audio->setSampleRate(16000);
          audio->setMonoOutput(true);
          audio->recordFromMic();
        } else if (!memcmp(lastKeys + 2, "502**", 5)) {    // **205##
          log_d("Easter egg = 205: stop recording WAV");
          char filename[100];
          sprintf(filename, "/audio_%02d%02d%02d%02d%02d%02d.pcm", ntpClock.getYear()-2000, ntpClock.getMonth(), ntpClock.getDay(), ntpClock.getHour(), ntpClock.getMinute(), ntpClock.getSecond());
          log_d("creating file %s", filename);
          audio->saveWavRecord(&SD, filename);
          audio->shutdown();

          // RINGTONE

        } else if (!memcmp(lastKeys + 2, "103**", 5)) {    // **301## - ringtone on
          log_d("Easter egg = 301: ringtone on");
          startRingtone();
        } else if (!memcmp(lastKeys + 2, "203**", 5)) {    // **302## - ringtone off
          log_d("Easter egg = 302: ringtone off");
          stopRingtone();
        }
#ifdef _WIPHONE_TEST_H_
        else {
          anyPressed = easteregg_tests(lastKeys, anyPressed);
        }
#endif
      }
    }

    // Turn on/off the keypad LEDs for 5s
    if (anyPressed) {
      msLastKeyPress = now;       // this is also used to idle keyboard event (when a symbol gets chosen via waiting)
      if (!keypadLedsOn) {
        // Turn on LEDs
        allDigitalWrite(KEYBOARD_LED, LOW);
        keypadLedsOn = true;
      }
    } else if (keypadLedsOn && elapsedMillis(now, msLastKeyPress, KEYPAD_LEDS_ON_MS)) {
      // Turn off LEDs
      allDigitalWrite(KEYBOARD_LED, HIGH);
      keypadLedsOn = false;
    }

    // Connect to WiFi — but never while the ROM transfer server is up (it may
    // be in softAP mode; an STA connect mid-association panics the chip) and
    // never while an auto-switch scan is in flight: connectToWiFi() hard-cycles
    // the radio (disconnect(true) + begin), which aborted every scan and made
    // the auto-switcher look completely dead.
    //
    // ⚠ WITH BACKOFF, mirroring the scanner's _discScans pattern: a hard radio cycle
    // every 20 s forever — plus the driver's own perpetual auto-reconnect between our
    // attempts — kept the radio actively probing/associating for a whole out-of-range
    // day, which is exactly the September hunt. The first five retries stay at 20 s
    // (stepping briefly out of range still rejoins fast); a sustained absence eases to
    // 3 min; the counter resets the moment anything connects; and a screen wake retries
    // immediately, so pulling the phone out of a pocket is never the slow path. While
    // eased, a plain WiFi.disconnect() 30 s after each failed attempt quiesces the
    // driver's auto-reconnect loop (the same move autoSwitchTick already makes), so the
    // radio genuinely rests between attempts instead of chewing mid-association.
    {
      static uint32_t s_wifiRetryFails = 0;
      static uint32_t s_wifiQuiesceAtMs = 0;
      static bool     s_prevScreenOnWifi = true;
      const bool screenOnNow = gui.state.screenBrightness > 0;
      const bool wokeNow = screenOnNow && !s_prevScreenOnWifi;
      s_prevScreenOnWifi = screenOnNow;

      if (wifiState.isConnected()) {
        s_wifiRetryFails = 0;
        s_wifiQuiesceAtMs = 0;
      }

      uint32_t retryMs = WIFI_RETRY_PERIOD_MS;
      if (s_wifiRetryFails >= 5) {
        retryMs = 180000u;              // clearly not a brief blip: ease off the radio
      }
      bool due = elapsedMillis(now, msLastWifiRetry, retryMs);
      if (wokeNow && !wifiState.isConnected() && !wifiState.userDisabled() &&
          (uint32_t)(now - lastWifiConnectAttemptMs()) >= 10000u) {
        /* Someone just picked the phone up: try NOW — unless a join started in the last
         * ten seconds, in which case hard-cycling the radio would abort an association
         * that was about to succeed and make the wake path SLOWER, not faster. */
        due = true;
      }

      /* ⚠ Block reconnect only while the transfer server is in softAP mode — that is the
       * mode where an STA connect mid-association panics the chip. In STA mode the server
       * is just a socket, and GATING RECONNECT STRANDED IT: a hotspot blink mid-upload
       * left the phone at NO_SSID with the page dead and no path back until the user
       * stopped the server (found live, 2026-08-20 — the "books upload locked up"). */
      const bool xferBlocksWifi = gbcXferOn() && xferUsingAP();
      if (!xferBlocksWifi && !wifiState.scanBusy() && wifiState.doReconnect() && !wifiState.isConnected() && due && !wifiState.userDisabled()) {
        bool _cp = false;
        TIME_STEP("connectToPreferred", _cp = wifiState.connectToPreferred());
        if (_cp) {
          log_d("Connecting to WiFi");
        } else {
          log_d("Not connecting to WiFi");
        }
        msLastWifiRetry = now;        // TODO: encapsulate into WiFiState
        if (s_wifiRetryFails < 1000) {
          s_wifiRetryFails++;
        }
        if (s_wifiRetryFails >= 5) {
          s_wifiQuiesceAtMs = now + 30000u;   // 30 s is every chance to associate
        }
      }

      if (s_wifiQuiesceAtMs && (int32_t)(now - s_wifiQuiesceAtMs) >= 0 &&
          !wifiState.isConnected() && !wifiState.scanBusy() && !xferBlocksWifi) {
        /* ⚠ Only quiesce an attempt that is OURS and STALE. connectToWiFi() is also
         * called by the auto-switcher and by a manual join in the networks app, and a
         * deadline armed 30 s ago knows nothing about them — disconnecting here would
         * abort a join that started milliseconds earlier (mid-DHCP still reads as "not
         * connected"). Any attempt younger than 30 s pushes the deadline out instead. */
        const uint32_t lastAttempt = lastWifiConnectAttemptMs();
        if ((uint32_t)(now - lastAttempt) < 30000u) {
          s_wifiQuiesceAtMs = lastAttempt + 30000u;
        } else {
          s_wifiQuiesceAtMs = 0;
          TIME_STEP("WiFi.disconnect(quiesce)", WiFi.disconnect());
          log_e("[wifi] eased retry: radio quiesced until the next attempt");
        }
      }
    }

    // WiFi auto-switch: background scan for the strongest saved network
    // (Networks.cpp). Runs whenever the phone isn't gaming / serving ROMs /
    // mid-AUDIO (a scan mid-call would glitch audio). NOTE: an earlier
    // allowlist of NotInited|Idle silently stopped all ticks once SIP left
    // Idle after connecting — block only genuine call-flow states.
    //
    // ⚠ HangUp and HangingUp are deliberately NOT blocking states. They are
    // TEARDOWN, and they STICK when the proxy is unreachable (the END key
    // enters HangUp from anywhere, no call needed — ino:1857): measured
    // 2026-08-15 (car log, 19 min at sip=6) and again 2026-08-20 (SIX HOURS
    // at sip=6, every scan blocked while the user's saved hotspot sat in
    // range — the deadlock: SIP waits for the network, and this guard made
    // the network wait for SIP). sipNeedsFullSpeed() had already recorded
    // which states are genuinely delicate; this guard now agrees.
    //
    // Belt and braces: with WiFi DISCONNECTED no state can be carrying audio,
    // so there is nothing a scan could glitch — the call gate only applies
    // while actually connected. That makes this deadlock structurally
    // impossible for ANY stuck state, present or future.
    {
      CallState cs = gui.state.sipState;
      bool audioDelicate = (cs == CallState::InvitingCallee || cs == CallState::InvitedCallee ||
                            cs == CallState::RemoteRinging  || cs == CallState::Call ||
                            cs == CallState::Accept         || cs == CallState::BeingInvited);
      bool callBusy = audioDelicate && wifiState.isConnected();
      // Same softAP-only rule as the reconnect gate above: an STA-mode transfer
      // server must not stop the machinery that would bring its network back.
      if (!gGbcActive && !(gbcXferOn() && xferUsingAP()) && !callBusy) {
        TIME_STEP("autoSwitchTick", wifiState.autoSwitchTick(gui.state.screenBrightness > 0));
      }
    }

#ifdef USE_VIRTUAL_KEYBOARD
    if (udpKeypad == NULL && wifiState.isConnected()) {
      log_d("Setting a connection");
      udpKeypad = new WiFiUDP();
      udpKeypad->begin(VIRTUAL_KEYBOARD_PORT); // need to check this for memory leak... does it get called repeatedly?
      IPAddress ipAddr = WiFi.localIP();
      log_d("Send UDP packets to:\n%d.%d.%d.%d:%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3], VIRTUAL_KEYBOARD_PORT);
    }
#endif

    // Trigger periodic event: APP_TIMER_EVENT
    if (gui.state.msAppTimerEventPeriod > 0) {
      if (elapsedMillis(now, gui.state.msAppTimerEventLast, gui.state.msAppTimerEventPeriod)) {
        redrawWhat |= gui.processEvent(now, APP_TIMER_EVENT);
        gui.state.msAppTimerEventLast = now;
      }
    }

    // Trigger scheduled events
    EventType evnt;
    while (evnt = gui.state.popEvent(now)) {
      redrawWhat |= gui.processEvent(now, evnt);
    }

    // Update message times
    if (updateMessageTimes) {
      gui.reloadMessages();
      updateMessageTimes = false;
    }

    /*if (wifiState.isConnected()) {
      const char* host = "phonetester";
      MDNSResponder mdnsResponder; // = new MDNSResponder();
      if (mdnsResponder.begin("WiPhone")) {
        IPAddress addr = mdnsResponder.queryHost(host, 2000);
        log_d("Address: %d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
      }
      // const char* host = "phonetester";
      // resolveMdns(host);
    }*/

    // Clock update
    if (ntpClock.isUpdated()) {
      if (waitingForClockUpdate) {
        updateMessageTimes = true;
        waitingForClockUpdate = false;
      }
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    } else if (elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS) || (ntpClock.getSecond() > 0 && elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS - ntpClock.getSecond() * 1000))) {
      // Tick at the beginning of next minute (or tick if not ticked for more than a minute)
      ntpClock.minuteTick(now);
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    }

//    // Turn OFF?
//    {
//      bool turnOff = gpioExtender.digitalRead(EXTENDER_PIN_B1) == HIGH ? true : false;
//      if (turnOff != lastTurnOff) {
//        lastTurnOff = turnOff;
//        log_d("TURN %s", turnOff ? "YES" : "NO");
//        if (! turnOff) {
//          log_d("BYE");
//          delay(2000);
//          allDigitalWrite(EXTENDER_PIN_B0, LOW);
//        }
//      }
//    }

    // Battery update
#ifdef WIPHONE_BOARD

    // Check battery state
    if (elapsedMillis(now, msLastBatt, BATTERY_CHECK_PERIOD_MS)) {
      msLastBatt = now;
      float v = gauge.readVoltage();
      if (v > 0.0) {
        gui.state.battVoltage = v;
      }
      float soc = gauge.readSocPrecise();
      if (soc > 0.0) {
        gui.state.battSoc = soc;
      }
      gui.state.battUpdated = true;
#if TF_CARD_DETECT_PIN >= 0
      /* ⚠ THESE TWO WERE PLAIN digitalRead() ON GPIO-EXTENDER PINS, WHICH READS THE
       * WRONG CHIP ENTIRELY. Both pins live on the SX1509, not on the ESP32:
       * TF_CARD_DETECT_PIN is EXTENDER_PIN(1) == 65 and BATTERY_CHARGING_STATUS_PIN is
       * EXTENDER_PIN(0) == 64 (EXTENDER_FLAG is 0x40). The ESP32 has GPIO 0-39, so both
       * were out of range — and digitalRead() does not reject them. It calls
       * gpio_get_level(), which for pin >= 32 evaluates `(in1.data >> (pin - 32)) & 1`;
       * the Xtensa shift masks its amount to 5 bits, so `>> 32` becomes `>> 0` and
       * `>> 33` becomes `>> 1`. The two reads therefore returned:
       *
       *     battCharged  <- GPIO 32  (USER_SERIAL_TX / MotorEN)
       *     cardPresent  <- GPIO 33  (I2S_WS_PIN, the I2S word-select clock)
       *
       * ⚠ THIS IS WHY `chg=` IS DEAD IN EVERY HEALTH SAMPLE. It was logged as "its own
       * small mystery" and then as a measured fact (0 across all 807 samples of the
       * 2026-08-23 run, through two unmistakable charging periods). The flag was never
       * reading the charger; it was reading an idle-low pin on the wrong die.
       *
       * ⚠ And cardPresent read the I2S word-select line, which is idle-low — so it
       * reported TRUE whether or not a card was seated, and would FLICKER while audio
       * plays, since WS toggles at the sample rate and the minute tick samples it at an
       * arbitrary phase. A card-dependent path could fail intermittently during music
       * for reasons nothing else could explain.
       *
       * allDigitalRead() is the accessor that honours EXTENDER_FLAG; it has existed in
       * Hardware.cpp all along and every other extender read in this file uses it or
       * goes to gpioExtender directly. */
      gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#else
      gui.state.cardPresent = false;
#endif
      /* ⚠ ACTIVE LOW. The charger IC's STAT output is open-drain: it PULLS THE LINE DOWN while
       * charging and releases it (pull-up → high) when it is not. This read was `== HIGH`, so
       * the flag has been inverted for the life of the project — on top of reading the wrong
       * GPIO entirely until 2026-08-24, which is what hid it.
       *
       * MEASURED, one continuous boot, one build stamp, no confound (2026-08-24):
       *     up=20  v=4.20  chg=0     on the charger
       *     up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
       *     up=25  v=4.11  chg=1     still on battery
       *     up=26  v=4.18  chg=0     <- replugged; voltage jumps back
       * chg=1 tracked NOT CHARGING throughout, and the voltage curve says so independently of
       * the flag. ⚠ Do not "correct" this back without repeating that unplug/replug run: it is
       * the only measurement that distinguishes a tapered charger from an inverted pin. */
      gui.state.battCharged = allDigitalRead(BATTERY_CHARGING_STATUS_PIN) == LOW ? true : false;

      /* ── HEALTH LINE ────────────────────────────────────────────────────────────
       * One line a minute, at log_e so it is visible in the field. It answers both of
       * the questions that keep coming up, over time rather than in a snapshot:
       *
       *   heap/min   a LEAK shows as `min` sliding down over hours. A steady min with
       *              occasional dips is just normal churn.
       *   largest    fragmentation: `free` can look healthy while the biggest single
       *              block is too small for the WiFi PHY, which is how this phone
       *              rebooted over a book.
       *   soc/v      the drain rate, which is the only honest way to tell whether a
       *              power change actually helped.
       *   up         minutes since boot -- an unplanned restart resets it. */
      static uint32_t s_minHeapEver = 0xFFFFFFFF;
      const uint32_t fh = ESP.getFreeHeap();
      if (fh < s_minHeapEver) {
        s_minHeapEver = fh;
      }
      char hl[200];
      snprintf(hl, sizeof(hl),
               "HEALTH up=%lumin heap=%u min=%u largest=%u psram=%u soc=%d%% v=%.2f chg=%d wifi=%d cpu=%luMHz scr=%d sip=%d",
               (unsigned long)(now / 60000), fh, s_minHeapEver, ESP.getMaxAllocHeap(),
               ESP.getFreePsram(), (int)round(soc), v,
               (int)gui.state.battCharged, (int)WiFi.status(),
               (unsigned long)getCpuFrequencyMhz(),
               (int)gui.state.screenBrightness, (int)gui.state.sipState);
      log_e("%s", hl);

      /* ⚠ The CARD gets a line a minute, not one every fifteen seconds like the console.
       * The battery poll runs at 15 s (BATTERY_CHECK_PERIOD_MS) and hanging the SD write
       * off it meant 240 open-write-close cycles an hour — measurable drain from the very
       * instrument meant to measure drain. Serial is free, so it keeps the faster rate;
       * the card does not. */
      static uint32_t s_lastCardLog = 0;
      if (s_lastCardLog == 0 || now - s_lastCardLog >= 60000u) {
        s_lastCardLog = now;
        healthLogLine(hl);

        /* Keypad health rides the same minute tick, but ONLY when something moved.
         * Dropped keypresses happen while a person is holding the phone and nobody is
         * watching a terminal, so the numbers have to reach the card; and a KEYS line
         * every minute forever would just push the interesting hours off the end of a
         * capped log. Silence here means the keypad had a clean minute. */
        char kl[176];
        int n = snprintf(kl, sizeof(kl), "KEYS ");
        keypadHealth(kl + n, (int)sizeof(kl) - n);
        static char s_lastKeys[176] = {0};
        if (strcmp(kl, s_lastKeys)) {
          strlcpy(s_lastKeys, kl, sizeof(s_lastKeys));
          log_e("%s", kl);
          healthLogLine(kl);
        }
      }

      log_d("Voltage/SOC = %.2f/%d%%", v, (int) round(soc));
      log_d("SD card = %d", gui.state.cardPresent);
      log_d("Charged = %d", gui.state.battCharged);
#if defined(POWER_CHECK) && POWER_CHECK >= 0
      /* gpioExtender indexes its OWN pins, so the flag has to come off first - the
       * working call at the top of loop() does `POWER_CHECK & ~EXTENDER_FLAG` and this
       * one did not, so it queried extender pin 66 and logged nonsense. */
      log_d("Power button = %d", gpioExtender.digitalRead(POWER_CHECK & ~EXTENDER_FLAG) == LOW ? 1 : 0);
#endif
      redrawWhat |= gui.processEvent(now, BATTERY_UPDATE_EVENT);

      // Power off at low battery to avoid unexpected behaviours
      if (v <= 3.3 && now >= 30000) {     // allow phone to work on low battery for 30 seconds
        powerOff();
        redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
      }
    }

    // Check for changes in WiFi strength
    if (elapsedMillis(now, msLastWiFiRssi, WIFI_CHECK_PERIOD_MS)) {
      msLastWiFiRssi = now;
      int rssi = WiFi.RSSI();
      if (GUI::wifiSignalStrength(gui.state.wifiRssi) != GUI::wifiSignalStrength(rssi)) {
        gui.state.wifiRssi = rssi;
        redrawWhat |= gui.processEvent(now, WIFI_ICON_UPDATE_EVENT);
      } else {
        gui.state.wifiRssi = rssi;
      }
    }

    // Check if USB is connected / charging the battery
    if (elapsedMillis(now, msLastUsbCheck, USB_CHECK_PERIOD_MS)) {
      msLastUsbCheck = now;
      //bool usbHere = gpioExtender.digitalRead(USB_POWER_DETECT_PIN)==LOW ? true : false;
      bool usbHere = digitalRead(BATTERY_PPR_PIN) == LOW ? true : false;
      usbConnected += usbHere ? 1 : 0;
      usbConnectedChecks += 1;
      if (usbHere != gui.state.usbConnected) {
        // USB cable status changed
        gui.state.usbConnected = usbHere;
        // If USB is connected -> enable blinking
        // otherwise -> disable blinking
        gui.state.battBlinkOn = usbHere;
        redrawWhat |= gui.processEvent(now, USB_UPDATE_EVENT);
      } else if (usbHere && !gui.state.battCharged && gui.state.battSoc < 100) {
#ifndef BATTERY_BLINKING_OFF
        // If blinking enabled -> trigger BATTERY_BLINK_EVENT every second
        gui.state.battBlinkOn = !gui.state.battBlinkOn;
        redrawWhat |= gui.processEvent(now, BATTERY_BLINK_EVENT);
        log_v("blinked");
#endif
      }
      if (!(usbConnectedChecks & 15)) {     // Dump check result to the logs occasionally
        log_d("USB = %d (%.1f%%), checks=%d", gui.state.usbConnected, 100.0 * usbConnected / usbConnectedChecks, usbConnectedChecks);
      }
    }

    // Power OFF
#ifdef WIPHONE_INTEGRATED_1_4
    if (powerButtonPressed && !poweringOff && elapsedMillis(now, msPowerOffStarted, 2500)) {
      powerOff();
      redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
    }
#endif // WIPHONE_INTEGRATED_1_4
#endif // WIPHONE_BOARD

    if (gui.state.inputCurKey && msLastKeyInput && elapsedMillis(now, msLastKeyInput, KEYPAD_IDLE_MS)) {
      log_i("keypad idle");
      redrawWhat |= gui.processEvent(now, KEYBOARD_TIMEOUT_EVENT);
      msLastKeyInput = 0;
    }

    // User serial processing
#ifdef USER_SERIAL
    // Read what has arrived to user UART
    while (userSerial.available() > 0) {
      char ch = userSerial.read();
      if (gGpsNmea) {
        /* GPS mode: NMEA feeds the fix, not the GUI. A completed RMC/GGA lands
         * in meshService, where resolveReference gives it to pos/sun/Places. */
        gGpsRawBuf[gGpsRawHead] = (uint8_t)ch;      // for `gps raw`
        gGpsRawHead = (gGpsRawHead + 1) % sizeof(gGpsRawBuf);
        gGpsRawSeen++;
        if (gGpsReader.feed(ch)) {
          const NmeaFix& fx = gGpsReader.fix();
          meshService.gpsUpdate(fx.valid, fx.latI, fx.lonI, fx.sats, fx.hdopX10);
        }
        continue;
      }
      log_d("USER SERIAL: %c", ch);
      gui.state.userSerialBuffer.put(ch);
    }
    if (!gGpsNmea &&
        gui.state.userSerialBuffer.size() > 0 && userSerialLastSize != gui.state.userSerialBuffer.size()) {
      // Trigger UART event
      log_d("User serial: %s", gui.state.userSerialBuffer.getCopy());
      redrawWhat |= gui.processEvent(now, USER_SERIAL_EVENT);
      userSerialLastSize = gui.state.userSerialBuffer.size();
    }
#endif // USER_SERIAL

    // GUI update
    if (redrawWhat & REDRAW_ALL) {
      gui.redrawScreen(redrawWhat & REDRAW_HEADER, redrawWhat & REDRAW_FOOTER, redrawWhat & REDRAW_SCREEN, redrawWhat & LOCK_UNLOCK);
    }

    // SIP CLIENT:

    //Added: check if disconnected from wif during call
    if (gui.state.hasSipAccount() && !wifiState.isConnected()) {
      if (sip.isBusy()) {
        log_d("Device disconnected from WIFI");
        log_d("Call will be terminated");

        // terminate audio session
        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        sip.wifiTerminateCall();  // wifi is disconnected but need to destroy this dialogue
        /* ⚠ A call that was still RINGING when WiFi dropped left the ringtone playing:
         * every other teardown path calls stopRingtone(), this one never did, so the
         * speaker kept announcing a call that no longer existed until something else
         * happened to stop it. Audio, motor and full-speed CPU, indefinitely. */
        stopRingtone();
        gui.exitCall();
        
        gui.state.setSipState(CallState::HungUp);
        appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        wifiTerminateSip = TERMINATE_OK;

        //gui.state.sipAccountChanged = true;
      } else {
        /*
         * in order to do ping and update staled state.
        */
        //sip.checkCall(now);
      }
    }

    /*check if remote party is disconnected during call*/
    if (rtpSilentPeriod == RTP_SILENT_ON) {

      rtpSilentPeriod = RTP_SILENT_OFF;

      if (rtpSilentCnt == 0x01) {
        rtpSilentCnt = 0x0;

        // Stop media session
        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        if (sip.isBusy()) {
          log_d("No RTP Packets From Remote Part");  // Send logs msgs with wifi disconnection

          sip.rtpSilent();
          gui.exitCall();

          gui.state.setSipState(CallState::HungUp);
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

          wifiTerminateSip = TERMINATE_OK;
        }
      }
      rtpSilentCnt++;
    }
    //    This is the "user-agent core", something that binds together SIP library, GUI, audio interfaces and message storage.
    //    It implements transitions between different call states.
    if (gui.state.hasSipAccount() && wifiState.isConnected() && sipMayPoll()) {
      if( wifiTerminateSip == TERMINATE_OK ) {
        gui.state.sipState == CallState::NotInited;
        wifiTerminateSip = 0x0;
      }
      if (gui.state.sipState == CallState::NotInited  ||  gui.state.sipAccountChanged) {
        log_d("SIP is going to init");
        // Connect to SIP proxy
        uint8_t mac[6];
        wifiState.getMac(mac);
        if (sip.init( gui.state.fromNameDyn,
                      gui.state.fromUriDyn,
                      gui.state.proxyPassDyn,
                      mac )) {
          sip.triedToMakeCallCounter = 0;
          log_d("Connected to SIP");
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (0) = %s", sip.isBusy() ? "NO" : "YES");
        } else {
          // Failed to connect to proxy
          log_e("failed to connect to SIP");
          gui.state.setSipState(CallState::Error);      // permanent error state  TODO
        }
        Random.feed(now);
        gui.state.sipEnabled = true;
        gui.state.sipAccountChanged = false;

      } else if (gui.state.sipState == CallState::Idle) {
        sip.triedToMakeCallCounter = 0;
        bool anySip = false;      // anything received?
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);     // TODO: all of this logic could be reorganized to call checkCall in one place and then process results according to the current state
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_INCOMING_CALL) {
            gui.state.setRemoteNameUri(sip.getRemoteName(), sip.getRemoteUri());
            gui.becomeCallee();
            gui.state.setSipState(CallState::BeingInvited);
            startRingtone();
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (Idle): 0x%x", res);
            gui.state.setSipState(CallState::Idle);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        bool isRegistered = sip.registrationValid(now);
        if (anySip || isRegistered != gui.state.sipRegistered) {
          log_d("setting reason @ CallState::Idle");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          // Force GUI to update screen if registration status has changed
          // NOTE: SIP registration can happen only in Idle state
          if (isRegistered != gui.state.sipRegistered) {
            // Ingicate successful registration
            gui.state.sipRegistered = isRegistered;
            // Allow GUI display it
            res |= gui.processEvent(now, REGISTRATION_UPDATE_EVENT);       // the app should decide whether to react to registration update
            /* ⚠ log_e, not log_d, and on BOTH edges — only log_e is compiled into this
             * build, so at log_d a phone that registers and one that never does look
             * identical on serial. Same lesson the SMS mirror learned. */
            log_e("SIP REGISTRATION -> %s", gui.state.sipRegistered ? "REGISTERED" : "lost");
          }
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        } else if (gui.state.outgoingMessages.size()) {

          // Send queued messages
          MessageData* msg = gui.state.outgoingMessages[0];
          if (msg) {
            auto err = sip.sendMessage(msg->getOtherUri(), msg->getMessageText());
            if (err == TINY_SIP_OK) {
              gui.flash.messages.setSent(*msg);
              delete msg;
              gui.state.outgoingMessages.remove(0);
            } else {
              log_e("message sending FAILED");
            }
          }

        }

      } else if (gui.state.sipState == CallState::BeingInvited) {

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              stopRingtone();
              log_d("call terminated @ BeingInvited");
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (BeingInvited): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        if (anySip) {
          log_d("setting reason @ CallState::BeingInvited");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::Accept) {

        log_v("Accepting call");

        stopRingtone();
        int res = sip.acceptCall();
        if (res == TINY_SIP_OK) {
          gui.state.setSipState(CallState::InvitedCallee);
        } else {
          log_e("could not accept call, err = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::Decline) {

        log_d("Declining call");

        stopRingtone();

        int res = sip.declineCall();
        if (res == TINY_SIP_OK) {
          gui.state.setSipState(CallState::HangingUp);
        } else {
          log_d("could not decline = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::InvitingCallee and gui.state.sipRegistered) {

        // Initialize / start call

        log_d("Calling: %s", gui.state.calleeUriDyn);
        if (strchr(gui.state.calleeUriDyn, '@') != NULL and  strlen(gui.state.calleeUriDyn)>0 and gui.state.sipRegistered) {
          sip.startCall(gui.state.calleeUriDyn, now);
          // Proceed to next state
          gui.state.setSipState(CallState::InvitedCallee);
          gui.redrawScreen(true, true, true, true);         // TODO: one of two special cases of redrawAll
        } else {
          log_e("sip callee unavailable");
          gui.state.setSipState(CallState::Idle);
        }

      } else if (gui.state.sipState == CallState::InvitedCallee) {
        // Audio session configs
        IPAddress rtpRemoteIP((uint32_t) 0);
        int rtpRemotePort = 0;
        uint16_t rtpLocalPort = 0;
        uint8_t audioFormat = TinySIP::NULL_RTP_PAYLOAD;

        bool callEstablished = false;
        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_CONFIRMED) {
            if (gui.state.sipState != CallState::Call) {  // change state only once
              log_d("call established");
              callEstablished = true;

              // Copy audio session configs
              if (!rtpRemoteIP.fromString(sip.getRemoteAudioAddr())) {
                rtpRemoteIP = resolveDomain(sip.getRemoteAudioAddr());
                if (!(uint32_t) rtpRemoteIP) {
                  log_e("couldn't parse IP address from \"%s\"", sip.getRemoteAudioAddr());
                }
              }
              rtpRemotePort = sip.getRemoteAudioPort();
              audioFormat = sip.getAudioFormat();
              log_d("  RTP rmt addr: %8X", (uint32_t) rtpRemoteIP);
              log_d("  RTP rmt port: %d", rtpRemotePort);
              log_d("  Audio format:  %d", audioFormat);

              if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != TinySIP::NULL_RTP_PAYLOAD) {
                rtpLocalPort = sip.getLocalAudioPort();
                log_d("  RTP loc port: %d", rtpLocalPort);
              }
              gui.state.setSipState(CallState::Call);
            }
          } else if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              gui.state.setSipState(CallState::Decline);
              log_d("call terminated @ InvitedCallee = %d", now);
              //gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            } else {
              gui.state.setSipState(CallState::Decline);
              log_d("call @ InvitedCallee is Declined");
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE: 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);

        // Update screen to show that a call was started
        //work by techtesh
        if (anySip) {
          log_d("setting reason @ CallState::InvitedCallee");
          gui.state.setSipReason(sip.getReason());
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

        // Start audio only after the screen is updated
        if (callEstablished) {
          // If audio configs are OK -> turn on audio (speaker & microphone) & start listening to audio port
          if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != TinySIP::NULL_RTP_PAYLOAD) {
            audio->openRtpConnection(rtpLocalPort);
            // This works the opposite of how you might expect. The ear speaker is what ends up getting muted. Probably need to remove after checking with Andriy.
            //audio->getVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);
            //audio->setVolume(-70, 6);                                   // max. volume for headphones, min. volume for speaker
            //audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, Audio::MuteVolume);    // mute loudspeaker for calls
            audio->sendRtpStreamFromMic(audioFormat, rtpRemoteIP, rtpRemotePort);
            audio->playRtpStream(audioFormat, rtpRemotePort);
          } else {
            log_e("audio session failure");
            gui.state.setSipReason("audio failed");
            // Force GUI to update screen
            appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
            gui.redrawScreen(false, false, true);
          }
        }

      } else if (gui.state.sipState == CallState::Call) {

        // Process any SIP requests quickly when call is established

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              audio->showAudioStats();
              audio->shutdown();
              log_d("call terminated by remote @ Call");
              audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            } else if(gui.state.sipState == CallState::HungUp) {
              log_d("Hang up call before remote party answers it");
              sip.declineCall();
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (2): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);
        if (anySip) {
          log_d("setting reason @ CallState::Call");
          gui.state.setSipReason(sip.getReason());
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangingUp) {

        bool anySip = false;      // anything received
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_TERMINATED) {
            if (gui.state.sipState != CallState::HungUp) {
              log_d("call terminated @ HangingUp");
              gui.state.setSipState(CallState::HungUp);
              msHungUp = now;
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE (3): 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);

        // (Timeout &) Update GUI
        if (anySip || elapsedMillis(now, msHangingUp, HANGUP_TIMEOUT_MS)) {
          if (anySip) {
            log_d("setting reason @ CallState::HangingUp");
            gui.state.setSipReason(sip.getReason());
          } else {
            log_d("hang up timeout");
            gui.state.setSipState(CallState::Idle);     // go straight to Idle on timeout
            log_d("caller free (1) = %s", sip.isBusy() ? "NO" : "YES");
          }

          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangUp) {

        // User request to hangup call -> send BYE / CANCEL request
        log_d("Terminating call");
        stopRingtone();
        // Stop media session
        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        int res = sip.terminateCall(now);
        if (res == TINY_SIP_OK) {
          msHangingUp = now;

          // Proceed to next state
          gui.state.setSipState(CallState::HangingUp);
        } else {
          log_d("terminating error = %d", res);
          gui.state.setSipState(CallState::HungUp);
          // Force GUI to update screen
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }


        // Go back to normal

        /*if (elapsedMillis(now, msHungUp, GUI::HUNGUP_TO_NORMAL_MS)) {

          log_d("hungup timeout: now = %d, msHungUp = %d", now, msHungUp);
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (2) = %s", sip.isBusy() ? "NO" : "YES");
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN, true);       // TODO: one of two special cases of redrawAll
        }*/

      } else if (gui.state.sipState == CallState::HungUp) {

        // Go back to normal

        if (elapsedMillis(now, msHungUp, GUI::HUNGUP_TO_NORMAL_MS)) {

          log_d("hungup timeout: now = %d, msHungUp = %d", now, msHungUp);
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (2) = %s", sip.isBusy() ? "NO" : "YES");
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN, true);       // TODO: one of two special cases of redrawAll
        }

      }

      // Check for incoming messages
      TextMessage* msg = NULL;
      if (msg = sip.checkMessage(now, ntpClock.getExactUtcTime(), ntpClock.isTimeKnown())) {
        log_v("message received");
        // Save message from external RAM into a file (part of message database)
        gui.flash.messages.saveMessage(msg->message, msg->from, msg->to, true, msg->useTime ? msg->utcTime : 0);    // time == 0 for unknown real time
        delete msg;
        // Pass event to GUI
        appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        notifyMessageArrived(now, gui.state.notifySipMode);   // a text arriving is news
      }
    } else {
      gui.state.sipRegistered = false;
    }

#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
    if (!gGbcActive && lora.loop()) {
      log_d("Received LoRa message");
      appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
      gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
    }

    if (gui.state.outgoingLoraMessages.size() > 0) {
      log_d("Sending LoRa message");
      // Send queued messages
      MessageData* msg = gui.state.outgoingLoraMessages[0];
      if (msg) {
        lora.send_message(msg->getOtherUri(), msg->getMessageText());
        gui.flash.messages.setSent(*msg);
        delete msg;
        gui.state.outgoingLoraMessages.remove(0);
      }
    }
#endif

    /* Music keeps playing while you are anywhere else in the phone. Audio::loop() is
     * already pumped from here and does the decoding; this only notices the end of a
     * track and starts the next one, which is why it lives outside MusicApp — that app
     * is deleted the moment you back out of its screen.
     *
     * ⚠ Skipped during a Game Boy session for the same reason the mesh is: that path
     * runs its own tight loop and the audio peripheral is handed to the emulator. */
    if (!gGbcActive) {
      musicPlayerLoop();
    }

    /* A call wins over music, always. The codec and I2S are single-occupancy, so ringing
     * or talking while a track plays gives you both at once and neither intelligibly.
     *
     * Edge-triggered on LEAVING the idle states, so it pauses once when the phone starts
     * ringing rather than every loop for the length of the call. Nothing resumes
     * afterwards on purpose: a phone that bursts into music the moment you hang up is
     * worse than one you press play on. */
    {
      const bool callBusy = sipCallActive();
      static bool wasCallBusy = false;
      if (callBusy && !wasCallBusy) {
        musicPlayerPause();
      }
      wasCallBusy = callBusy;
    }

    // Meshtastic background service tick (non-blocking). If a new message
    // arrived, notify the GUI so an open Channel view refreshes live, raise the
    // status-bar unread flag, and show a brief popup banner on any screen.
    if (!gGbcActive && meshService.loop()) {
      log_d("Received Meshtastic message");
      gui.state.meshUnread = true;
      gui.processEvent(now, NEW_MESSAGE_EVENT);   // let an open Mesh view rebuild
      // Full repaint so the unread icon shows immediately on any screen.
      gui.redrawScreen(true, true, true);
      const MeshMessage* nm = meshService.getMessage(0);   // newest = the arrival
      if (nm) {
        const MeshNode* n = meshService.findNode(nm->from);
        char title[32];
        snprintf(title, sizeof(title), "Mesh: %s", n ? n->name : "new message");
        gui.showMeshPopup(title, nm->text);                // drawn on top of app
        meshPopupActive = true;
        meshPopupShownMs = now;
      }
      notifyMessageArrived(now, gui.state.notifyMeshMode);
    }

    /* A position or waypoint arrived: refresh an open Places/Nodes view so the
     * user is never sitting on "No places heard yet" after camp has already
     * arrived — the book-sync 'receiver shows nothing' trap in new clothes.
     * QUIET on purpose: no popup, no unread icon, no buzz — places are ambient
     * state, not news. */
    if (!gGbcActive && meshService.takePlacesNews()) {
      gui.processEvent(now, NEW_MESSAGE_EVENT);
    }

    // Stop the notification vibration after its brief pulse.
    if (meshVibroActive && elapsedMillis(now, meshVibroStartMs, MESH_VIBRO_MS)) {
      allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
      meshVibroActive = false;
    }

    // One-shot pop: the PCM player loops, so stop it by timer once it has
    // played through. Use ceasePlayback (NOT shutdown) — shutdown()'s codec
    // power-down on the shared I2C bus disrupts the vibro motor extender.
    /* ⚠ audio->restore() IS NOT OPTIONAL — see Audio::preserve() for the whole story.
     * playPop() reconfigures six output parameters and this teardown used to put none of
     * them back, so one mesh notification left the phone at 8 kHz, mono, loudspeaker-forced
     * and at full volume permanently. That made music play mono out of the loudspeaker with
     * headphones in, and it is the same monoOut=true that paces the Game Boy at 50%.
     * Restoring is correct in BOTH exits below: it puts back the pre-pop state, and a call
     * starting will then configure whatever it needs on top. */
    if (meshPopPlaying) {
      if (gui.state.sipState == CallState::Call || gui.state.ringing) {
        audio->restore();
        meshPopPlaying = false;
      } else if (elapsedMillis(now, meshPopStartMs, MESH_POP_MS)) {
        audio->ceasePlayback();
        audio->restore();
        meshPopPlaying = false;
      }
    }

    // Auto-dismiss the mesh popup: repaint the screen to erase it after a moment.
    if (meshPopupActive && elapsedMillis(now, meshPopupShownMs, MESH_POPUP_MS)) {
      meshPopupActive = false;
      gui.redrawScreen(true, true, true, true);
    }

//    if (gui.state.ledPleaseTurnOn) {
//      allDigitalWrite(EXTENDER_PIN_B2, HIGH);
//      gui.state.ledPleaseTurnOn = false;
//    }
//    if (gui.state.ledPleaseTurnOff) {
//      allDigitalWrite(EXTENDER_PIN_B2, LOW);
//      gui.state.ledPleaseTurnOff = false;
//    }

    // RINGTONE

    if (gui.state.ringing && gui.state.sipState != CallState::Call) {

      /* ⚠ Both blocks below must respect the ringer mode. isEof() answers TRUE when nothing is
       * playing, so in vibrate-only mode the rewind branch would fire every pass, permanently
       * resetting the vibro state machine and leaving the motor dead — the exact opposite of
       * what "vibrate only" is for. */
      const bool tonePlaying = (gui.state.ringerMode == ControlState::RINGER_RING_AND_VIBRATE);

      // Check if end of ringtone was reached

      if (tonePlaying && audio->isEof()) {

        // Rewind the ringtone file
        audio->rewind();

        // Restart vibro motor logic
        gui.state.vibroOn = false;
        gui.state.vibroToggledMs = now;
        gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        allDigitalWrite(KEYBOARD_LED, HIGH);

      }

      // Control vibro motor

      if (gui.state.ringerMode != ControlState::RINGER_SILENT &&
          elapsedMillis(now, gui.state.vibroToggledMs, gui.state.vibroNextDelayMs)) {

        gui.state.vibroToggledMs = now;
        gui.state.vibroOn = !gui.state.vibroOn;
        gui.state.vibroNextDelayMs = gui.state.vibroOn ? gui.state.vibroOnPeriodMs : gui.state.vibroOffPeriodMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, gui.state.vibroOn ? HIGH : LOW);
        allDigitalWrite(KEYBOARD_LED, gui.state.vibroOn ? LOW : HIGH);

      }
    }

    // Audio
    //msProfile.add(micros()-loopTime);
//    uint32_t loopTime = micros();
//    if (!msProfileStart) msProfileStart = loopTime;
    audio->loop();

//    // Profiler
//    msProfile.add(micros()-loopTime);
//    if (loopTime - msProfileStart > 2500000) {
//      uint32_t sum = 0, mx = 0, cnt = 0;

//      log_d("Profile:");
//      for (auto it = msProfile.iterator(); it.valid(); ++it) {
//        uint32_t c1 = *it;
//        DEBUG_PRINTF("%d ", c1);      // Before audio
//        ++it;
//        log_d("%d", *it - c1);        // After audio
//        sum += *it;
//        mx = (mx < *it) ? *it : mx;
//        cnt++;
//      }
//      log_d("Loop: avg = %.1f us, max = %d", (float)sum/cnt, mx);
//      msProfile.purge();
//      msProfileStart = 0;

//      log_d("Audio profile:");
//      for (auto it = audio->profile.iterator(); it.valid(); it++) {
//        uint32_t tot = (*it).time[6] - (*it).time[0];
//        sum += tot;
//        mx = (mx < tot) ? tot : mx;
//        cnt++;
//        it->show();
//      }
//      log_d("Audio loop: sum = %d, avg = %.1f us, max = %d, cnt = %d", sum, (float)sum/cnt, mx, cnt);
//      audio->profile.purge();

//      #ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
//      showRunTimeStats();
//      #endif
//    }

    // Theoretically, gives time for modem sleep? Allows to consume less power?
    //delay(1);   // sleep for 1 millisecond
    //vTaskDelay(1);    // sleep for a single tick: allows context switch
    /* ── RUN SLOW WHEN NOBODY IS WATCHING ─────────────────────────────────────────
     * The chip is pinned at 240 MHz by platformio.ini because the Game Boy needs it, and
     * it stayed there in a pocket with the screen off. At 80 MHz the core draws roughly
     * half as much, and the phone spends most of its life doing nothing but listening.
     *
     * Full speed whenever anything would notice:
     *   - the screen is on (someone is looking, and redraws should feel instant)
     *   - music is playing (the decoder has headroom at 240 and less of it at 80)
     *   - a call is up (RTP is a hard 20 ms deadline)
     *   - the Game Boy is running (the reason 240 was pinned in the first place)
     *   - an upload is in progress (throughput is already marginal on a weak link)
     *
     * ⚠ 80 is the floor, not lower: WiFi needs at least 80 MHz to work at all. And the
     * switch only happens on a CHANGE — calling this every pass would be its own drain.
     *
     * ⚠ Not applied while the radio or I2S are mid-transfer by design: everything in the
     * "full speed" list above covers those cases, so the frequency only ever moves when
     * the phone is genuinely idle. */
    bool idleTickStretch = false;
    {
      /* ── TWO PREDICATES, NOT ONE ──────────────────────────────────────────────
       * `busy` used to decide BOTH the CPU frequency AND the idle tick below, and that
       * coupling is a trap: loosening it to save power under a lit screen would also
       * hand a lit screen the 5 ms tick, slowing every pump in the loop. They are
       * separated here deliberately. `busy` keeps its ORIGINAL meaning and remains the
       * ONLY input to the tick. Do not re-merge them.
       *
       * The frequency question is different from the tick question: a lit screen that is
       * merely being STARED AT does not need 240 MHz, while one being redrawn does. On
       * this hardware that is a safe distinction to make, because a redraw is bus-bound,
       * not CPU-bound - the whole 153,600-byte frame goes out over a 40 MHz SPI that does
       * NOT scale with the core clock (in this core `calculateApb()` returns a flat
       * 80 MHz for any CPU frequency >= 80, so an 80<->240 move reprograms no peripheral
       * at all). ⚠ Never target below 80 MHz: there the clock source becomes the XTAL,
       * APB really does follow, and every SPI divider in the phone silently goes wrong.
       *
       * UI_WORK_HOLD_MS is a transition-rate knob, NOT a responsiveness one: the wake
       * path already repaints the screen before this gate is reached, and any keypress
       * raises the clock on the pass that handles it, so the frame a person actually
       * perceives is never the slow one. Set UI_IDLE_DOWNCLOCK to 0 to restore the
       * previous behaviour exactly. */
/* ⚠ 0 = OFF, AND IT IS OFF ON PURPOSE. Tried on hardware 2026-08-22 and BACKED OUT
 * the same evening: Nick reported "the menu is a bit laggy and doesn't pick up every
 * button push". Missed input is not a tuning problem, it is a break, so this does not
 * get dialled down to 160 - it gets turned off until it can be done without touching
 * the input path.
 *
 * WHAT THE EVIDENCE ACTUALLY SAID, because it was not what I expected: the new
 * "screen idle" state NEVER FIRED - zero occurrences in the whole session, only "busy"
 * and "idle". So the phone was never running its menus at 80 MHz, and the lag did not
 * come from the low clock at all. It came from the other half: cpuRaiseForUi() calls
 * setCpuFrequencyMhz() from INSIDE the key-drain loop, and a PLL switch landing in the
 * middle of a keypad read is exactly how a keypress goes missing. The level was never
 * the problem; the SWITCHING was, which is why 160 MHz would not have helped.
 *
 * If this is ever revisited: raise the clock somewhere that is not the input path, and
 * MEASURE the saving first - it was never quantified, and the backlight probably
 * dominates screen-on draw anyway. Trading proven input handling for an unmeasured
 * saving was the wrong bet and this comment is here so nobody repeats it. */
#define UI_IDLE_DOWNCLOCK   0
#define UI_WORK_HOLD_MS     2000
      /* 🛑 gGpsNmea HOLDS THE CLOCK, AND IT IS NOT ABOUT PERFORMANCE — IT AVOIDS A HARD
       * DEADLOCK IN THE ARDUINO CORE. Every HardwareSerial registers uart_on_apb_change()
       * (esp32-hal-uart.c:225). On APB_BEFORE_CHANGE that callback disables the UART RX
       * interrupt and then drains the hardware FIFO into the 256-byte RX queue with
       * xQueueSend(..., 1) — a ONE-TICK BLOCK, and CONFIG_FREERTOS_HZ is 1000, so one
       * millisecond per byte once that queue is full. The queue's only consumer is this
       * loop, which is at that moment inside the callback, so nothing can ever free a slot.
       *
       * 🔑 THE ARITHMETIC IS WHAT DECIDES IT, AND 115200 IS THE WRONG SIDE OF IT:
       *      115200 -> 11.52 bytes arrive per ms, 1 drains per ms -> +10.5/ms, never exits
       *        9600 ->  0.96 bytes arrive per ms, 1 drains per ms -> converges, exits
       * So with the plate's GPS streaming, ANY setCpuFrequencyMhz() call can park this task
       * forever. MEASURED 2026-08-25: the loop stops, other tasks keep printing, CPU 1 sits
       * in IDLE1 (it BLOCKS, it does not spin), and no watchdog fires because only core 0's
       * idle task is checked. ⚠ And note esp32-hal-cpu.c:190-196 fires the callback on ANY
       * frequency change, even 80<->240 where APB does not actually move — the guard there
       * covers only the register write, not the callback.
       *
       * ⚠ Why it looked like "mostly on screen unlock": the wake REPAINT (~30.7 ms of SPI)
       * runs BEFORE the clock is raised, so ~354 bytes pile into a 256-byte queue and the
       * threshold is crossed every time. Screen-OFF is preceded by a backlight write, not a
       * repaint — a short pass, queue not full, which is why the downclock so often survived
       * and sent this hunt after the wrong transition.
       *
       * The cost is idle power while the GPS is on, and that is the right trade against a
       * phone that stops answering its buttons. */
      const bool busy = (gui.state.screenBrightness > 0) ||
                        gGbcActive ||
                        gGpsNmea ||            // see the deadlock note above — NOT perf
                        xferOn() ||
                        musicPlayerIsPlaying() ||
                        sipNeedsFullSpeed();   // NOT sipCallActive() — see the note on it
      /* Anything with a deadline stays at 240 REGARDLESS of the screen: the emulator, the
       * transfer server, audio playback and a live SIP session. Only the screen term is
       * relaxed, and only while nothing is being drawn. */
      const bool hardBusy = gGbcActive || gGpsNmea || xferOn() || musicPlayerIsPlaying() || sipNeedsFullSpeed();
      extern volatile uint32_t gUiWorkMs;      // GUI.cpp: stamped by every redraw
      const bool uiWorking = (uint32_t)(millis() - gUiWorkMs) < UI_WORK_HOLD_MS;
#if UI_IDLE_DOWNCLOCK
      const bool wantFull = hardBusy || (gui.state.screenBrightness > 0 && uiWorking);
#else
      const bool wantFull = busy;
#endif
      const uint32_t wantMhz = wantFull ? 240 : 80;
      if (wantMhz != gCpuCurMhz) {
        setCpuFrequencyMhz(wantMhz);
        gCpuCurMhz = wantMhz;
        /* Label the reason the ACTIVE predicate gave, not the experimental one - with
         * UI_IDLE_DOWNCLOCK off, a lit screen goes to 240 via `busy`, and calling that
         * "screen idle" in the log is how you mislead the next person reading it. */
        log_e("CPU %luMHz (%s)", (unsigned long)wantMhz,
#if UI_IDLE_DOWNCLOCK
              hardBusy ? "busy" : (gui.state.screenBrightness > 0 ? "screen idle" : "idle"));
#else
              busy ? "busy" : "idle");
#endif
      }
      /* The same predicate decides the TICK below — plus one extra gate: while the LAN
       * mirror poll is mid-transfer its state machine advances one bounded step per pass,
       * so stretching the tick then would slow a live download 5x for no saving worth
       * having. */
      idleTickStretch = !busy && !smsMirrorPollBusy();
    }

    /* ── LET THE CPU ACTUALLY IDLE ────────────────────────────────────────────────
     * This was taskYIELD(), which hands over to any ready task and comes straight back
     * — the main loop spun flat out at 240 MHz forever, even with the screen off and
     * nothing happening. The CPU never got to sleep, which is most of why the battery
     * went nowhere.
     *
     * vTaskDelay(1) blocks for one tick instead, so the FreeRTOS idle task runs and
     * executes the Xtensa `waiti` instruction, halting the core until the next interrupt.
     *
     * ⚠ One TICK, not one millisecond of dead time in anything that matters. Audio
     * decodes ahead in ~24 ms frames, RTP packets are 20 ms, and the keypad is
     * interrupt-driven, so none of them notice. The Game Boy runs its own inner loop and
     * never reaches this line.
     *
     * FIVE ticks when the phone is verifiably idle (same predicate as the CPU-MHz gate,
     * plus the mirror poll being between transfers): every pass still runs SIP socket
     * checks, the mesh poll gate, and a dozen timer compares, so 1,000 passes a second
     * kept the core awake 10-30% of idle time doing nothing. At 5 ms the worst added
     * latency is 4 ms — on keypad wake (interrupt-latched), on a LoRa packet that spent
     * >100 ms on air, on sockets that already ride network jitter. Nothing a person or a
     * protocol can perceive, for roughly a fifth of the idle wakeups. */
    vTaskDelay(idleTickStretch ? 5 : 1);

    //esp_sleep_enable_timer_wakeup(1000000); // 0.001 s
    //int ret = esp_light_sleep_start();
    //if (ret != ESP_OK) printf("light sleep error: %d\r\n", ret);
  }
}

void powerOff() {
  log_i("POWER OFF");
  allDigitalWrite(POWER_CONTROL, HIGH);       // produces a power down from software
  msPowerOffStarted = millis();
  poweringOff = true;
}
