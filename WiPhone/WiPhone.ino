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
#include "Test.h"
#include "meshtastic_service.h"
#include "music_player.h"
#include "app_gbc_xfer.h"
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

  // Start audio
  audio->start();

  // Start playing ringtone
  if (!audio->playRingtone(&SPIFFS)) {
    log_d("ERROR: could not play file in SPIFFS");
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
// Per-key timestamp of the last release, for UI debouncing: a "new press"
// within 75ms of the same key's release is switch bounce (or a stale hold
// re-report), not a human double-tap — humans can't re-tap under ~100ms.
static uint32_t keyLastUpMs[32];
// Keys the UI considers held down. UI key events are strictly edge-triggered
// off THIS mask: one event per physical press, no matter how long it's held
// and no matter what the chip re-reports meanwhile. Only a real release event
// (or 350ms of heartbeat silence = lost release) re-arms the key. This is
// deliberately separate from keypadState, which other heuristics may clear
// mid-hold (that's what double-clicked and auto-repeated the menus).
static uint32_t uiKeyDown = 0;
/* The boot banner, held until the first health tick can commit it. ⚠ It cannot be written
 * when it is produced: setup() logs the reset reason before the card is known good, so
 * writing there would silently drop the one line that explains a restart. */
static char bootLine[120] = {0};
// True while the Game Boy emulator app is running. The main loop then skips the
// mesh/LoRa polling so the emulator has the SPI bus and CPU to itself.
volatile bool gGbcActive = false;
// Sticky "pressed since last read" mask for the emulator. Set on every key-down
// here (the main loop sees every press); the emulator ORs it into the held state
// and clears it each frame, so a fast tap isn't missed between its slow polls.
volatile uint32_t gGbcKeyLatch = 0;
RingBuffer<char> keypadBuff(KEYBOARD_BUFFER_LENGTH);        // TODO: maybe used cbuf.h from ESP32 stack?

void IRAM_ATTR keyboardInterrupt() {
  // This function is intentionally minimal
  // (for example, adding a Serial output here produces ISR crashes)
  keypadToRead = 1;
}

// Function that is called after interrupt occurs (not within interrupt)
// Connect to keypad scanners via I2C and decode the keyboard events into buffer keypadBuff
void keyboardRead() {
  uint8_t key;
  uint32_t mask;
  uint32_t newState = 0;
  char c;
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
      // Do NOT decode the garbage byte (key==0 decodes as a CALL release) and
      // do NOT abandon whatever is still in the chip's FIFO: the chip auto-
      // clears its INT line, so a dropped event never announces itself again —
      // that was a missed tap, or a button stuck down until the next press.
      // Leave the flag set so the next loop pass retries the read.
      log_d("keypad err code=%d", err);
      keypadToRead = 1;
      break;
    }

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

    // Decode "pressed/released" bit
    if (key & SN7326_PRESSED) {
      newState |= mask;         // reported (still) pressed — count it even if already known
      if (mask) {
        keyLastSeenMs[__builtin_ctz(mask)] = millis();   // heartbeat for the stale sweep
      }
      if (!(keypadState & mask)) {
        keypadState |= mask;
        gGbcKeyLatch |= mask;   // remember the press for the emulator's next poll
        //Serial.print(c); Serial.println(" pressed");

        // UI events are edge-triggered: fire only if this key wasn't already
        // considered down (uiKeyDown re-arms on a real release), and not
        // within 75ms of its own release (contact bounce). This kills menu
        // double-clicks and hold-to-autoscroll. UI-only: the keypadState and
        // latch updates above are untouched, so game input is unchanged.
        bool uiSuppress = mask == 0 ||
                          (uiKeyDown & mask) ||
                          (millis() - keyLastUpMs[__builtin_ctz(mask)] < 40);
        uiKeyDown |= mask;

        // Process key if there is still space left in the key buffer
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
        keyLastUpMs[__builtin_ctz(mask)] = millis();   // for the UI debounce window
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

#ifdef WIPHONE_INTEGRATED
  pinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  pinMode(BATTERY_PPR_PIN, INPUT);
#endif // WIPHONE_INTEGRATED
#ifdef WIPHONE_BOARD
  pinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
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
  userSerial.begin(USER_SERIAL_BAUD, USER_SERIAL_CONFIG, USER_SERIAL_RX, USER_SERIAL_TX);
  //allDigitalWrite(EXTENDER_PIN_B0, HIGH);     // TODO: why do we do this?
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
  snprintf(bootLine, sizeof(bootLine), "BOOT reset_reason=%d heap=%u psram=%u",
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
          gui.state.dimming = ini["screen"].getIntValueSafe("dimming", 0) > 0;
          gui.state.dimLevel = ini["screen"].getIntValueSafe("dim_level", 15);
          gui.state.dimAfterMs = ini["screen"].getIntValueSafe("dim_after_s", 20)*1000;
          gui.state.sleeping = ini["screen"].getIntValueSafe("sleeping", 0) > 0;
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

        // Load keypad locking config
        gui.state.locking = ini.hasSection("lock") ? ini["lock"].getIntValueSafe("lock_keyboard", 0) : 1;
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
#define HEALTH_LOG_MAX  (96 * 1024)
#define HEALTH_LOG_KEEP (32 * 1024)   // ~250 lines, ~4 hours, retained across a trim

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

extern void gbcXferHandleClient();   // ROM-transfer web server pump (no-op when off)
extern bool gbcXferOn();             // true while the transfer server is running

void loop() {
  while (1) {
    uint32_t now = millis();
    gbcXferHandleClient();

    // Stale-held-key sweep, driven by the SN7326's ~40ms held-key re-reports
    // (LONGPRESS_DELAY(1)): a key silent for 350ms lost its release event.
    // - uiKeyDown: always (re-arms the UI edge-trigger so the key isn't dead)
    // - keypadState (game input): game mode only, as before
    // If holds ever break rhythmically at ~350ms, the chip's re-report isn't
    // periodic at this setting — revert to DELAY(2) and 5200ms here.
    uint32_t swept = uiKeyDown | (gGbcActive ? keypadState : 0);
    for (uint32_t st = swept; st; st &= st - 1) {
      int b = __builtin_ctz(st);
      if (now - keyLastSeenMs[b] > 350) {
        uiKeyDown &= ~(1u << b);
        if (gGbcActive) {
          keypadState &= ~(1u << b);
        }
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

    // During gameplay, also drain the keypad FIFO periodically WITHOUT an
    // interrupt: the chip auto-clears its INT line after 10ms, so an event
    // arriving mid-service (or lost to an I2C error) can sit in the FIFO with
    // no edge left to announce it — a missed tap, or a button stuck down
    // until the next keypress. Reading an empty FIFO is a no-op.
    static uint32_t msLastKeyDrain = 0;
    if (!toRead && gGbcActive && elapsedMillis(now, msLastKeyDrain, 150)) {
      toRead = 1;
    }

    // Read all the keys to buffer
    if (toRead) {
      msLastKeyDrain = now;
      keyboardRead();
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
    if (!gbcXferOn() && !wifiState.scanBusy() && wifiState.doReconnect() && !wifiState.isConnected() && elapsedMillis(now, msLastWifiRetry, WIFI_RETRY_PERIOD_MS) && !wifiState.userDisabled()) {
      if (wifiState.connectToPreferred()) {
        log_d("Connecting to WiFi");
      } else {
        log_d("Not connecting to WiFi");
      }
      msLastWifiRetry = now;        // TODO: encapsulate into WiFiState
    }

    // WiFi auto-switch: background scan for the strongest saved network
    // (Networks.cpp). Runs whenever the phone isn't gaming / serving ROMs /
    // in CALL activity (a scan mid-call would glitch audio). NOTE: an earlier
    // allowlist of NotInited|Idle silently stopped all ticks once SIP left
    // Idle after connecting — block only genuine call-flow states.
    {
      CallState cs = gui.state.sipState;
      bool callBusy = (cs == CallState::InvitingCallee || cs == CallState::InvitedCallee ||
                       cs == CallState::RemoteRinging  || cs == CallState::Call ||
                       cs == CallState::HangUp         || cs == CallState::HangingUp ||
                       cs == CallState::Accept         || cs == CallState::BeingInvited);
      if (!gGbcActive && !gbcXferOn() && !callBusy) {
        wifiState.autoSwitchTick(gui.state.screenBrightness > 0);
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
      gui.state.cardPresent = digitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#else
      gui.state.cardPresent = false;
#endif
      gui.state.battCharged = digitalRead(BATTERY_CHARGING_STATUS_PIN) == HIGH ? true : false;

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
      }

      log_d("Voltage/SOC = %.2f/%d%%", v, (int) round(soc));
      log_d("SD card = %d", gui.state.cardPresent);
      log_d("Charged = %d", gui.state.battCharged);
#if defined(POWER_CHECK) && POWER_CHECK >= 0
      log_d("Power button = %d", gpioExtender.digitalRead(POWER_CHECK) == LOW ? 1 : 0);
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
      log_d("USER SERIAL: %c", ch);
      gui.state.userSerialBuffer.put(ch);
    }
    if (gui.state.userSerialBuffer.size() > 0 && userSerialLastSize != gui.state.userSerialBuffer.size()) {
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
    if (gui.state.hasSipAccount() && wifiState.isConnected()) {
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
            log_d("SIP EVENT_REGISTERED = %d", gui.state.sipRegistered);
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
      // Quiet notification "pop" — play unless we're in an active call/ringtone.
      if (gui.state.sipState != CallState::Call && !gui.state.ringing && !meshPopPlaying) {
        bool played = audio->playPop(&SPIFFS);
        log_e("Mesh pop: played=%d", played);
        if (played) {
          meshPopPlaying = true;
          meshPopStartMs = now;
        }
      }
      // Short silent buzz too (skip if the ringtone owns the motor).
      if (!gui.state.ringing) {
        allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
        meshVibroActive = true;
        meshVibroStartMs = now;
        log_e("Mesh vibro: on");
      }
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

      // Check if end of ringtone was reached

      if (audio->isEof()) {

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

      if (elapsedMillis(now, gui.state.vibroToggledMs, gui.state.vibroNextDelayMs)) {

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
    {
      const bool busy = (gui.state.screenBrightness > 0) ||
                        gGbcActive ||
                        xferOn() ||
                        musicPlayerIsPlaying() ||
                        sipNeedsFullSpeed();   // NOT sipCallActive() — see the note on it
      const uint32_t wantMhz = busy ? 240 : 80;
      static uint32_t curMhz = 240;
      if (wantMhz != curMhz) {
        setCpuFrequencyMhz(wantMhz);
        curMhz = wantMhz;
        log_e("CPU %luMHz (%s)", (unsigned long)wantMhz, busy ? "busy" : "idle");
      }
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
     * never reaches this line. */
    vTaskDelay(1);

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
