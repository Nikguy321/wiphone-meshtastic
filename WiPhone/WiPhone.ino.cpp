# 1 "/var/folders/zj/c7fhzdv11892ggb43jt4z0k80000gn/T/tmpefx1ok7y"
#include <Arduino.h>
# 1 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
# 15 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#include <dummy.h>

#include <HTTPClient.h>
#include <HTTPUpdate.h>






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
#include "esp_task_wdt.h"
#include "Test.h"
#include "meshtastic_service.h"
#include "music_player.h"
#include "app_gbc_xfer.h"
#include "sms_mirror_poll.h"
#include "sms_mirror_rx.h"
#include "serial_cmd.h"
#include "mp3_stream.h"
#include "src/assets/pop_sound.h"

static bool been_in_verify = false;

#ifndef WIPHONE_PRODUCTION
#include "Test.h"
#endif

extern "C" bool verifyOta() {
  log_d("In verify ota");
  been_in_verify = true;
  return true;
}

static Ota ota("");

GUI gui;
uint32_t chipId = 0;


#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
static Lora lora;
#endif



SN7326 keypad(SN7326_I2C_ADDR_BASE, I2C_SDA_PIN, I2C_SCK_PIN);
CW2015 gauge(CW2015_I2C_ADDR, I2C_SDA_PIN, I2C_SCK_PIN);

#if defined(MOTOR_DRIVER) && MOTOR_DRIVER == 8833
DRV8833 motorDriver = DRV8833();
#endif

#ifdef USER_SERIAL
HardwareSerial userSerial(2);
int userSerialLastSize = 0;







#include "nmea.h"
#include <Preferences.h>
bool gGpsNmea = false;
uint32_t gGpsBaud = GPS_SERIAL_BAUD_DEFAULT;
NmeaReader gGpsReader;



bool gGpsBaudPending = false;







static uint8_t gGpsRawBuf[64];
static uint8_t gGpsRawHead = 0;
static uint32_t gGpsRawSeen = 0;
void gpsApplyBaud(bool gpsOn);
int gpsRawSnapshot(uint8_t* out, int cap);
void audio_test();
void startRingtone();
void stopRingtone();
void IRAM_ATTR headphoneInterrupt();
void headphoneServiceInterrupt();
int keypadTrace(char* out, int cap);
int keypadHealth(char* out, int cap);
bool uiInjectKey(char c);
void IRAM_ATTR keyboardInterrupt();
void keyboardRead(bool polled);
void keyboardUdpRead();
void IRAM_ATTR gpioExtenderInterrupt();
bool gpioExtenderServiceInterrupt();
void setup();
static bool healthLogTrim();
int healthDump(uint32_t lastBytes);
void healthLogLine(const char* line);
static bool sipMayPoll();
static bool sipNeedsFullSpeed();
static bool sipCallActive();
void smsMirrorNotifyArrival();
void loop();
void powerOff();
#line 114 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
void gpsApplyBaud(bool gpsOn) {
  userSerial.updateBaudRate(gpsOn ? gGpsBaud : USER_SERIAL_BAUD);
}


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



Audio* audio;

void audio_test() {
  log_e("AUDIO TEST");
  if (audio->start()) {
    log_d("audio: started");
    audio->playRingtone(&SPIFFS);

  } else {
    log_e("audio: failed");
  }
}
# 163 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
void startRingtone() {




  const uint8_t mode = gui.state.ringerMode;
  const bool playTone = (mode == ControlState::RINGER_RING_AND_VIBRATE);
  const bool doVibrate = (mode != ControlState::RINGER_SILENT);

  if (playTone) {

    audio->start();


    if (!audio->playRingtone(&SPIFFS)) {
      log_d("ERROR: could not play file in SPIFFS");
    }
  } else {
    log_d("ringer: tone suppressed (mode %d)", (int)mode);
  }


  gui.state.vibroOn = false;
  gui.state.vibroToggledMs = millis();


  gui.state.vibroOnPeriodMs = 500;
  gui.state.vibroOffPeriodMs = 2500;
  gui.state.vibroDelayMs = gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs;
  gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;


  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);


  IniFile ini("/ringtone.ini");
  if (ini.load() && !ini.isEmpty()) {
    gui.state.vibroOnPeriodMs = ini[0].getIntValueSafe("vibro_on", gui.state.vibroOnPeriodMs);
    gui.state.vibroOffPeriodMs = ini[0].getIntValueSafe("vibro_off", gui.state.vibroOffPeriodMs);
    gui.state.vibroDelayMs = ini[0].getIntValueSafe("delay", gui.state.vibroOnPeriodMs + gui.state.vibroOffPeriodMs);
    gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;

    log_d("vibro on = %d", gui.state.vibroOnPeriodMs);
    log_d("vibro off = %d", gui.state.vibroOffPeriodMs);
    log_d("vibro delay = %d", gui.state.vibroDelayMs);
  }


  gui.state.ringing = true;
}






void stopRingtone() {
  audio->shutdown();
  gui.state.ringing = false;
  gui.state.vibroOn = false;
  allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
  allDigitalWrite(KEYBOARD_LED, HIGH);
}



volatile bool headphoneEvent = false;

void IRAM_ATTR headphoneInterrupt() {
  headphoneEvent = true;
}


void headphoneServiceInterrupt() {
#ifdef HEADPHONE_DETECT_PIN
  bool headphones = allDigitalRead(HEADPHONE_DETECT_PIN);
#else
  bool headphones = false;
#endif
  log_d("Headphones event = %d", headphones);
  audio->setHeadphones(headphones);
  headphoneEvent = false;
}



#define KEYBOARD_BUFFER_LENGTH 13

volatile uint8_t keypadToRead = 0;
uint32_t keypadState = 0;



static uint32_t keyLastSeenMs[32];
# 268 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
static uint32_t keyLastUpMs[32];






#define KEY_BOUNCE_MS 25u






static uint32_t uiKeyDown = 0;
# 305 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define KEYPAD_POLL_MS 40u
#define KEYPAD_POLL_TAIL_MS 1000u






#define KEY_HOLD_GAP_MS 100u


static uint32_t msLastKeypadActivity = 0;




static uint32_t kcPollDrained = 0;
static uint32_t kcBatchRescued = 0;
static uint32_t kcBounceKilled = 0;
static uint32_t kcStaleSwept = 0;
static uint32_t kcGapRescued = 0;
static uint32_t kcGapMissed = 0;
static uint32_t kcBuffFull = 0;
static uint32_t kcI2cErr = 0;
static uint32_t kcEmptyPolls = 0;
static uint32_t kcReleaseFixed = 0;
static uint32_t kcRelAmbiguous = 0;
# 341 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define KEYTRACE_N 64
static uint8_t ktByte[KEYTRACE_N];
static uint16_t ktGap[KEYTRACE_N];
static uint8_t ktFlag[KEYTRACE_N];
static uint8_t ktHead = 0;
static uint32_t ktLastMs = 0;
static inline void keyTrace(uint8_t b, bool polled, bool unknown, bool dropped = false) {
  (void)dropped;
  const uint32_t nowMs = millis();
  uint32_t d = ktLastMs ? (nowMs - ktLastMs) : 0;
  ktLastMs = nowMs;
  ktByte[ktHead] = b;
  ktGap[ktHead] = d > 65535 ? 65535 : (uint16_t)d;
  ktFlag[ktHead] = (polled ? 1 : 0) | (unknown ? 2 : 0) | (dropped ? 4 : 0);
  ktHead = (ktHead + 1) % KEYTRACE_N;
}
# 373 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
int keypadTrace(char* out, int cap) {
  int n = 0;
  for (int i = 0; i < KEYTRACE_N && n < cap - 1; i++) {
    const uint8_t k = (ktHead + i) % KEYTRACE_N;
    if (!ktGap[k] && !ktByte[k] && !ktFlag[k]) {
      continue;
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
                  (unsigned long)kcGapRescued, (unsigned long)kcGapMissed,
                  (unsigned long)kcStaleSwept, (unsigned long)kcPollDrained,
                  (unsigned long)kcBatchRescued, (unsigned long)kcBuffFull,
                  (unsigned long)kcI2cErr, (unsigned long)kcEmptyPolls);
}



static char bootLine[120] = {0};




static uint32_t gLastKeyMs = 0;






#define TIME_STEP(name,call) do { const uint32_t _t0 = millis(); call; const uint32_t _d = millis() - _t0; if (_d > 150) { log_e("SLOW STEP: %s took %u ms (wifi=%d) - the whole loop waited on it", name, (unsigned)_d, (int)WiFi.status()); } } while (0)


volatile bool gGbcActive = false;



volatile uint32_t gGbcKeyLatch = 0;
RingBuffer<char> keypadBuff(KEYBOARD_BUFFER_LENGTH);
# 438 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
bool uiInjectKey(char c) {
  if (keypadBuff.full()) {
    return false;
  }
  keypadBuff.put(c);
  return true;
}

void IRAM_ATTR keyboardInterrupt() {


  keypadToRead = 1;
}
# 459 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
void keyboardRead(bool polled) {
  uint8_t key;
  uint32_t mask;
  uint32_t newState = 0;
  char c;
  bool firstRead = true;



  static uint32_t msPrevRead = 0;
  const uint32_t sinceLastRead = msPrevRead ? (millis() - msPrevRead) : 0xFFFFFFFFu;
  msPrevRead = millis();





  uint32_t releasedThisBatch = 0;
  keypadToRead = 0;

  do {
    key = 0;
    sn7326_err_t err = keypad.readKey(key);
    if (err == SN7326_ERROR_BUSY) {
      log_d("i2c reset");
      keypad.reset();
    }
    if (err) {
      kcI2cErr++;





      log_d("keypad err code=%d", err);
      keypadToRead = 1;
      break;
    }
# 514 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (polled && firstRead && key == 0 &&
        !((keypadState | uiKeyDown) & WIPHONE_KEY_MASK_CALL)) {
      kcEmptyPolls++;




      keyTrace(key, true, false, true);
      return;
    }
    if (polled && firstRead) {





      kcPollDrained++;
    }
    firstRead = false;


    switch (key & B111111) {
#ifndef WIPHONE_KEYBOARD

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
      mask = 0;
    }
    keyTrace(key, polled, mask == 0);
# 697 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (!polled && !(key & SN7326_PRESSED) && (key & B111111) == 0) {
      const uint32_t held = uiKeyDown | keypadState;
# 721 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      if (held && !(held & WIPHONE_KEY_MASK_CALL) && (held & (held - 1)) == 0) {
        mask = held;
        kcReleaseFixed++;
      } else if (held & (held - 1)) {
        kcRelAmbiguous++;
      }
    }


    if (key & SN7326_PRESSED) {
      newState |= mask;
      uint32_t sinceSeen = 0;
      if (mask) {
        uint8_t b = __builtin_ctz(mask);
        sinceSeen = millis() - keyLastSeenMs[b];
        keyLastSeenMs[b] = millis();
      }

      if (mask && !(keypadState & mask)) {
        keypadState |= mask;
        gGbcKeyLatch |= mask;

      }
# 759 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      {
# 777 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
        const bool watching = sinceLastRead <= 120;
        if (mask && (uiKeyDown & mask) && sinceSeen > KEY_HOLD_GAP_MS && watching) {
# 796 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
          kcGapRescued++;
        }

        const bool alreadyDown = mask && (uiKeyDown & mask);
        const bool sameBatchRetap = mask && (releasedThisBatch & mask);
        const bool looksLikeBounce = mask &&
                                     (millis() - keyLastUpMs[__builtin_ctz(mask)] < KEY_BOUNCE_MS);
        bool uiSuppress = mask == 0 || alreadyDown || looksLikeBounce;
        if (sameBatchRetap && !alreadyDown) {
# 813 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
          kcBatchRescued++;
        }
        if (looksLikeBounce) {
          kcBounceKilled++;
        }
        if (alreadyDown && sinceSeen > KEY_HOLD_GAP_MS) {



          kcGapMissed++;
        }
# 832 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
      if (mask) {
        keypadState &= ~mask;
        uiKeyDown &= ~mask;

        for (uint32_t m = mask; m; m &= m - 1) {
          keyLastUpMs[__builtin_ctz(m)] = millis();
        }
        releasedThisBatch |= mask;

      }
    }
  } while (key & SN7326_MORE);






  if (!gGbcActive && newState < keypadState) {
    keypadState = newState;
  }



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

    for (int i = 0; i < cb && !keypadBuff.full(); i++) {
      if (buff[i] == 10 || buff[i] == 13 || !buff[i]) {
        continue;
      }
      keypadBuff.put(buff[i]);
    }
  }
}
#endif





bool powerButtonPressed = false;
bool poweringOff = false;
volatile bool gpioExtenderEvent = false;

void IRAM_ATTR gpioExtenderInterrupt() {

  gpioExtenderEvent = true;
}


bool gpioExtenderServiceInterrupt() {
  gpioExtenderEvent = false;
  bool powerButton = gpioExtender.digitalRead(POWER_CHECK & ~EXTENDER_FLAG) == LOW;

  if (powerButton != powerButtonPressed) {
    powerButtonPressed = powerButton;
    if (powerButton) {
      keypadBuff.put(WIPHONE_KEY_END);
    }
    return true;
  }
  return false;
}



void setup() {

  const uart_config_t uart_config = {
    .baud_rate = SERIAL_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
  };

  int RX_BUF_SIZE = 1024;

  uart_param_config(UART_NUM_0, &uart_config);
  uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);

  for(int i=0; i<17; i=i+8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }

  log_i("\r\nChip id: %X %d %d", chipId, ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));
  log_i("Firmware version: %s", FIRMWARE_VERSION);


  gauge.connect();
#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gui.state.gaugeInited = gauge.configure();
  log_d("\r\nBattery gauge: %s\n", gui.state.gaugeInited ? "OK" : "FAILED");
  delay(10);
#endif

  log_v("Free memory after gauge: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


#ifdef WIPHONE_INTEGRATED_1_4
  if (gpioExtender.begin()) {
    log_v("extender succ");
    gui.state.extenderInited = true;

    allPinMode(POWER_CHECK, INPUT);
    allPinMode(TF_CARD_DETECT_PIN, INPUT);
    allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);

    allPinMode(KEYBOARD_LED, OUTPUT);
    allPinMode(VIBRO_MOTOR_CONTROL, OUTPUT);
    allPinMode(POWER_CONTROL, OUTPUT);

    allDigitalWrite(POWER_CONTROL, LOW);
    allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
    allDigitalWrite(KEYBOARD_LED, HIGH);
  } else {
    log_e("extender failed");
    gui.state.extenderInited = false;
  }
#else
#ifdef WIPHONE_INTEGRATED_1_3
  {
    auto err = gpioExtender.config(

                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,

                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1 | EXTENDER_PIN_FLAG_B7
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#else
#ifdef WIPHONE_INTEGRATED_1
  {
    auto err = gpioExtender.config(

                 EXTENDER_PIN_FLAG_A2 | EXTENDER_PIN_FLAG_B1,

                 EXTENDER_PIN_FLAG_A0 | EXTENDER_PIN_FLAG_A1 | EXTENDER_PIN_FLAG_B0 | EXTENDER_PIN_FLAG_B1
               );
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gui.state.extenderInited = (err == SN7325_ERROR_OK);
    err = gpioExtender.setInterrupts(EXTENDER_PIN_FLAG_A2);
    if (err != SN7325_ERROR_OK) {
      log_d("GPIO extender error = %d", err);
    }
    gpioExtender.showState();
  }
#endif
#endif
#endif

  log_v("Free memory after integrated: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


  void* p = heap_caps_malloc(100000, MALLOC_CAP_SPIRAM);
  if (p != NULL) {
    gui.state.psramInited = true;
    freeNull((void **) &p);
  }

  log_v("Free memory after psram: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


#if defined(POWER_CONTROL) && POWER_CONTROL >= 0
  allPinMode(POWER_CONTROL, OUTPUT);
  allDigitalWrite(POWER_CONTROL, LOW);
#endif

  log_v("Free memory after power: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if defined(POWER_CHECK) && POWER_CHECK >= 0
#ifdef WIPHONE_INTEGRATED_1_4
  log_d("enabling interrupt (rev1.4)");
  gpioExtender.enableInterrupt(2, FALLING);
#else
#ifdef WIPHONE_INTEGRATED_1_3
  log_d("enabling interrupt input (rev1.3)");
  allPinMode(POWER_CHECK, INPUT_PULLUP);
#endif
#endif
#endif

  Random.feed(micros());


  if (SPIFFS.begin()) {
    log_d("SPI filesystem mounted");



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


  log_d("Initializing screen");
  gui.init(lcdLedOnOff);
  gui.redrawScreen(false, false, true);

  log_v("Free memory after gui: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

#if LCD_LED_PIN >= 0

  log_d("LCD_LED_PIN = %d", LCD_LED_PIN);
  allPinMode(LCD_LED_PIN, OUTPUT);
#if GPIO_EXTENDER == 1509
  gpioExtender.ledDriverInit(LCD_LED_PIN ^ EXTENDER_FLAG);
#else
  allPinMode(LCD_LED_PIN, OUTPUT);
#endif
  gui.toggleScreen();
#endif

  log_v("Free memory after lcd led: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)


  allPinMode(TF_CARD_DETECT_PIN, INPUT);


  gui.state.battVoltage = gauge.readVoltage();
  gui.state.battSoc = gauge.readSocPrecise();
  log_d("Voltage = %.2f", gui.state.battVoltage);
  log_d("SOC = %.1f", gui.state.battSoc);
#endif

  log_v("Free memory after sd spiffs: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


  if (gui.state.battVoltage < 3.1) {
    powerOff();
    gui.processEvent(millis(), POWER_OFF_EVENT);
  }




#if defined(WIPHONE_BOARD) || defined(WIPHONE_INTEGRATED)
  gauge.showVersion();
#endif



  if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_FREQUENCY)) {
    log_d("Card mounted");
  }
# 1212 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  gui.loadWallpaper();
# 1224 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#if TF_CARD_DETECT_PIN >= 0
  gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#endif



  {

    sn7326_err_t err = keypad.config();
    if (err != SN7326_ERROR_OK) {
      log_d("keypad error = %d", err);
    }
    gui.state.scannerInited = (err == SN7326_ERROR_OK);
  }

  log_v("Free memory after keypad: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));


#ifdef WIPHONE_INTEGRATED_1_4
  rmtTxInit(AMPLIFIER_SHUTDOWN, false);
#else
#ifdef WIPHONE_INTEGRATED_1_3

#endif
#endif


#ifdef I2S_MCLK_GPIO0
  {


    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
    REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 0);
  }
#endif


  log_v("Free memory after config files: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));
# 1272 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#ifdef WIPHONE_INTEGRATED
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);
  allPinMode(BATTERY_PPR_PIN, INPUT);
#endif
#ifdef WIPHONE_BOARD
  allPinMode(BATTERY_CHARGING_STATUS_PIN, INPUT);

#endif

#if defined(KEYBOARD_RESET_PIN) && KEYBOARD_RESET_PIN > 0
  pinMode(KEYBOARD_RESET_PIN, OUTPUT);
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);
  delay(1);
  digitalWrite(KEYBOARD_RESET_PIN, LOW);
  delay(1);
  digitalWrite(KEYBOARD_RESET_PIN, HIGH);
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







#ifdef USER_SERIAL

  {
    Preferences p;
    p.begin("wpmesh", true);
    gGpsNmea = p.getBool("gpsen", false);
    gGpsBaud = p.getUInt("gpsbaud", GPS_SERIAL_BAUD_DEFAULT);
    p.end();
  }
# 1343 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  userSerial.begin(USER_SERIAL_BAUD, USER_SERIAL_CONFIG,
                   USER_SERIAL_RX, USER_SERIAL_TX);
  if (gGpsNmea) {
    gGpsBaudPending = true;
  }
#endif
# 1364 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  log_e("BOOT: reset_reason=%d heap=%u psram=%u",
        (int)esp_reset_reason(), ESP.getFreeHeap(), ESP.getFreePsram());
# 1375 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
    wifiState.disable();
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




  if (!ota.hasJustUpdated() && ota.userRequestedUpdate()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  } else if (!ota.hasJustUpdated() && (ota.autoUpdateEnabled() || ota.userRequestedUpdate()) && ota.updateExists()) {
    gui.drawOtaUpdate();
    ota.doUpdate();
  }

  ota.setUserRequestedUpdate(false);
#endif

  static Audio audio_local(true, I2S_BCK_PIN, I2S_WS_PIN, I2S_MOSI_PIN, I2S_MISO_PIN);
  audio = &audio_local;
  gui.state.codecInited = !audio->error();


  {
    CriticalFile ini(Storage::ConfigsFile);
    if ((ini.load() || ini.restore()) && !ini.isEmpty()) {
      if (ini[0].hasKey("v") && !strcmp(ini[0]["v"], "1")) {


        if (ini.hasSection("audio")) {
          int8_t speakerVol, headphonesVol, loudspeakerVol;
          audio->getVolumes(speakerVol, headphonesVol, loudspeakerVol);
          speakerVol = ini["audio"].getIntValueSafe("speaker_vol", speakerVol);
          headphonesVol = ini["audio"].getIntValueSafe("headphones_vol", headphonesVol);
          loudspeakerVol = ini["audio"].getIntValueSafe("loudspeaker_vol", loudspeakerVol);
          audio->setVolumes(speakerVol, headphonesVol, loudspeakerVol);
          log_d("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
          log_i("loaded volume: earpiece = %d dB, headphones = %d dB, loudspeaker = %d dB", speakerVol, headphonesVol, loudspeakerVol);
        }


        if (ini.hasSection("time")) {
          float tz = ini["time"].getFloatValueSafe("zone", 0);
          ntpClock.setTimeZone(tz);
        }


        if (ini.hasSection("screen")) {
          gui.state.brightLevel = ini["screen"].getIntValueSafe("bright_level", 100);







          gui.state.dimming = ini["screen"].getIntValueSafe("dimming", 1) > 0;
          gui.state.dimLevel = ini["screen"].getIntValueSafe("dim_level", 15);
          gui.state.dimAfterMs = ini["screen"].getIntValueSafe("dim_after_s", 20)*1000;
          gui.state.sleeping = ini["screen"].getIntValueSafe("sleeping", 1) > 0;
          gui.state.sleepAfterMs = ini["screen"].getIntValueSafe("sleep_after_s", 30)*1000;
          gui.state.screenBrightness = gui.state.brightLevel-1;
          gui.processEvent(0, 0);
        } else {
          gui.state.brightLevel = 100;
          gui.state.dimming = true;
          gui.state.dimLevel = 15;
          gui.state.dimAfterMs = 20000;
          gui.state.sleeping = true;
          gui.state.sleepAfterMs = 30000;
        }
# 1517 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
#endif

  log_v("Free memory after audio: %d %d", ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_32BIT));

  ntpClock.startUpdates();


#if defined(LORA_MESSAGING) && !defined(MESHTASTIC_PHY)
  lora.setup();
#endif
# 1546 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  meshService.setCardPresent(gui.state.cardPresent);
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
# 1581 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  esp_task_wdt_init(20, false);
  esp_task_wdt_add(NULL);
  log_e("WDT: loop task subscribed, 20 s, print-not-panic (diagnostic build)");

  printf("\r\nBooted\r\n");
# 1597 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
# 1620 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#ifdef XFER_AUTOSTART
  {
    static const XferConfig AUTO_CFG = {
      MUSIC_DIR, "Add music", ".mp3,.wav", "tracks", "download.mp3", "WiPhone-Music"
    };




    for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
      delay(250);
    }
    log_e("XFER_AUTOSTART: wifi=%d ip=%s", (int)WiFi.status(),
          WiFi.localIP().toString().c_str());
    xferStart(&AUTO_CFG);
    log_e("XFER_AUTOSTART: uploader on at %s ap=%d", xferAddr(), (int)xferUsingAP());
  }
#endif
# 1650 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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



TinySIP sip;
char lastKeys[7];
uint32_t msLastKeyPress = 0;
uint32_t msLastKeyInput = 0;
uint32_t msPowerOffStarted = 0;
uint32_t msHangingUp = 0;
uint32_t msHungUp = 0;
uint32_t msLastRtpPacket = 0;
uint32_t msLastBatt = 0;
uint32_t msLastUsbCheck = 0;
uint32_t msLastMinute = 0;
uint32_t msLastWifiRetry = -WIFI_RETRY_PERIOD_MS;
uint32_t msLastWiFiRssi = -WIFI_CHECK_PERIOD_MS;
uint8_t rtpSilentCnt = 0x0;
bool keypadLedsOn = false;
bool updateMessageTimes = false;
bool waitingForClockUpdate = true;
uint8_t wifiTerminateSip = 0x0;

int8_t restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol;

uint32_t usbConnected = 0;
uint32_t usbConnectedChecks = 0;

bool lastTurnOff = true;




uint32_t last_lora_send = 0;


#define MESH_POPUP_MS 2500u
static bool meshPopupActive = false;
static uint32_t meshPopupShownMs = 0;
# 1799 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define SLEEP_CHORD_MS 2000u
#define SLEEP_CHORD_MASK (WIPHONE_KEY_MASK_BACK | WIPHONE_KEY_MASK_SELECT)
# 1823 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define HEALTH_LOG_PATH "/health.log"
#define HEALTH_LOG_TMP "/health.tmp"
# 1837 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define HEALTH_LOG_MAX (256 * 1024)
#define HEALTH_LOG_KEEP (128 * 1024)
# 1856 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
static bool healthLogTrim() {
  File in = SD.open(HEALTH_LOG_PATH, FILE_READ);
  if (!in) {
    return false;
  }
  const size_t total = in.size();
  if (total <= HEALTH_LOG_KEEP) {
    in.close();
    return true;
  }
  in.seek(total - HEALTH_LOG_KEEP);
  while (in.available() && in.read() != '\n') {
    ;
  }

  SD.remove(HEALTH_LOG_TMP);
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
      ok = false;
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
# 1917 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
    while (f.available() && f.read() != '\n') { }
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





  const int wrote = snprintf(buf, sizeof(buf),
                             "\n--- end · %u bytes · cap %u · trims to newest %u ---\n",
                             (unsigned)total, (unsigned)HEALTH_LOG_MAX, (unsigned)HEALTH_LOG_KEEP);
  if (wrote > 0) {
    uart_write_bytes(UART_NUM_0, buf, (size_t)wrote);
  }
  return (int)total;
}



void healthLogLine(const char* line) {
  if (!gui.state.cardPresent) {
    return;
  }
  File f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
  if (!f) {
    return;
  }




  if (f.size() > HEALTH_LOG_MAX) {
    f.close();
    if (!healthLogTrim()) {
      SD.remove(HEALTH_LOG_PATH);
    }
    f = SD.open(HEALTH_LOG_PATH, FILE_APPEND);
    if (!f) {
      return;
    }
  }

  if (bootLine[0]) {
    f.println("");
    f.println(bootLine);
    bootLine[0] = '\0';
  }
  f.println(line);
  f.close();
}
# 2027 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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
      return true;
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
      return true;
    default:
      return false;
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

#define MUSIC_F2_HOLD_MS 500
static uint32_t msF2Down = 0;
static bool f2Fired = false;
static uint32_t msChordStart = 0;
static bool chordFired = false;



#define MESH_POP_MS 280u
static bool meshPopPlaying = false;
static uint32_t meshPopStartMs = 0;


#define MESH_VIBRO_MS 180u
static bool meshVibroActive = false;
static uint32_t meshVibroStartMs = 0;
# 2103 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
static void notifyMessageArrived(uint32_t now, uint8_t mode);



void smsMirrorNotifyArrival() {
  notifyMessageArrived(millis(), gui.state.notifySipMode);
}

static void notifyMessageArrived(uint32_t now, uint8_t mode) {
# 2120 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  if (mode == ControlState::RINGER_SILENT) {
    log_e("NOTIFY: silent (mode=%d) - badge only", (int)mode);
    return;
  }
  const bool callBusy = (gui.state.sipState == CallState::Call);
# 2140 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
  if (!gui.state.ringing) {
    allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
    meshVibroActive = true;
    meshVibroStartMs = now;
    log_e("NOTIFY: buzz %u ms (mode=%d)", (unsigned)MESH_VIBRO_MS, (int)mode);
  } else {




    log_e("NOTIFY: suppressed (ringing latched)");
  }
  if (mode == ControlState::RINGER_RING_AND_VIBRATE) {




    if (!(callBusy || gui.state.ringing || meshPopPlaying)) {
      const bool played = audio->playPop(&SPIFFS, gui.state.notifyVolume);
      if (played) {
        meshPopPlaying = true;
        meshPopStartMs = now;
      }
    }
  }
}


extern void gbcXferHandleClient();
extern bool gbcXferOn();
# 2183 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
static uint32_t gCpuCurMhz = 240;






__attribute__((unused)) static void cpuRaiseForUi() {
  if (gCpuCurMhz != 240) {
    setCpuFrequencyMhz(240);
    gCpuCurMhz = 240;
  }
}

void loop() {
  while (1) {
    esp_task_wdt_reset();

    uint32_t now = millis();
#ifdef USER_SERIAL



    if (gGpsBaudPending) {
      gGpsBaudPending = false;
      gpsApplyBaud(true);
      gGpsReader.reset();
      log_e("GPS: NMEA reader ON (user UART %d/%d @ %u) - retuned from loop()",
            USER_SERIAL_RX, USER_SERIAL_TX, (unsigned)gGpsBaud);
    }
#endif
# 2228 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {
      static uint32_t sLastPassMs = 0;
      if (sLastPassMs) {
        const uint32_t took = now - sLastPassMs;
        if (took > 250) {
          log_e("LOOP STALL: %u ms in one pass (scr=%d cpu=%uMHz wifi=%d) - WiFi/keypad/screen "
                "were all frozen for this long", (unsigned)took, (int)gui.state.screenBrightness,
                (unsigned)(getCpuFrequencyMhz()), (int)WiFi.status());






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




    meshService.setUiIdle(gLastKeyMs == 0 || (uint32_t)(now - gLastKeyMs) > 3000);
    meshService.setCardPresent(gui.state.cardPresent);
    gbcXferHandleClient();
    serialCmdLoop();
# 2273 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    smsMirrorPollLoop(sipMayPoll());
# 2286 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {
      static bool s_mirrorUiPending = false;
      static uint32_t s_mirrorUiLastMs = 0;
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
# 2318 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {





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
# 2365 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    static uint32_t s_lastSweepMs = 0;
    const bool loopWasStalled = s_lastSweepMs && (now - s_lastSweepMs) > 100;
    s_lastSweepMs = now;
    for (uint32_t st = loopWasStalled ? 0 : (uiKeyDown | keypadState); st; st &= st - 1) {
      int b = __builtin_ctz(st);
      if (now - keyLastSeenMs[b] > 350) {




        if (uiKeyDown & (1u << b)) {
          kcStaleSwept++;
        }
        uiKeyDown &= ~(1u << b);
        keypadState &= ~(1u << b);




      }
    }
# 2404 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (!gGbcActive && musicPlayerCurrent() >= 0) {
      if (uiKeyDown & WIPHONE_KEY_MASK_F2) {
        if (!msF2Down) {
          msF2Down = now;
          f2Fired = false;
        } else if (!f2Fired && now - msF2Down >= MUSIC_F2_HOLD_MS) {
          f2Fired = true;
          musicPlayerPrev();
        }
      } else {
        if (msF2Down && !f2Fired) {
          musicPlayerNext();
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
        chordFired = true;
        if (gui.state.screenBrightness > 0) {
          gui.sleepScreen();
        }
      }
    } else {
      msChordStart = 0;
      chordFired = false;
    }





    appEventResult redrawWhat = DO_NOTHING;


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


    if (headphoneEvent) {
      headphoneServiceInterrupt();
    }




    uint8_t toRead = keypadToRead;
# 2490 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    static uint32_t msLastKeyDrain = 0;
    bool polledRead = false;
    if (!toRead && gGbcActive && elapsedMillis(now, msLastKeyDrain, 150)) {
      toRead = 1;
      polledRead = true;
    }


    if (toRead) {
      msLastKeyDrain = now;
      keyboardRead(polledRead);
    }
#ifdef USE_VIRTUAL_KEYBOARD
    keyboardUdpRead();
#endif


    EventType keyPressed;
    bool anyPressed = false;
    while (!keypadBuff.empty()) {

      keyPressed = keypadBuff.get();
      if (keyPressed) {
        gLastKeyMs = millis();
      }
#if UI_IDLE_DOWNCLOCK
      if (keyPressed) {
        cpuRaiseForUi();
      }
#endif



      Random.feed(rotate5(now) ^ keyPressed);


      for (uint8_t k = sizeof(lastKeys) - 1; k > 0; k--) {
        lastKeys[k] = lastKeys[k - 1];
      }
      lastKeys[0] = keyPressed;



      if (!anyPressed && gui.state.inputType == InputType::AlphaNum) {
        msLastKeyInput = now;
      }
      anyPressed = true;


#ifndef STEAL_THE_USER_BUTTONS
# 2562 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      const bool musicLoaded = !gGbcActive && musicPlayerCurrent() >= 0;
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
        keyPressed = 0;
      }

      if (keyPressed == WIPHONE_KEY_END && !gGbcActive) {


        gui.state.setSipState(CallState::HangUp);
      }




#else
      if (keyPressed == WIPHONE_KEY_F1) {
        gui.toggleScreen();


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


        esp_sleep_enable_ext0_wakeup((gpio_num_t)KEYBOARD_INTERRUPT_PIN,0);

        log_d("begin delay");
        delay(500);
        log_d("begin light sleep");
        delay(10);

        esp_light_sleep_start();
        log_d("awake!!!");





      }
#endif


      redrawWhat |= gui.processEvent(now, keyPressed);





      if (!memcmp(lastKeys, "##", 2)) {

        if (!memcmp(lastKeys + 2, "101**", 5)) {
          log_d("Easter egg = 101: starting an SIP client");
          gui.state.setSipState(CallState::InvitingCallee);
        } else if (!memcmp(lastKeys + 2, "301**", 5)) {
          log_d("Easter egg = 103: send register request");
          sip.registration();
        } else if (!memcmp(lastKeys + 2, "601**", 5)) {

          log_d("Easter egg = 106: send message test. Add a sip account in WiPhone.ino to use this test");
          sip.sendMessage("sip:user@host.com", "Hello from WiPhone");
        } else if (!memcmp(lastKeys + 2, "701", 5)) {
          log_d("Easter egg = 107: test motor and blink LED");
          allDigitalWrite(VIBRO_MOTOR_CONTROL, HIGH);
          allDigitalWrite(KEYBOARD_LED, LOW);
          delay(2500);
          allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
          allDigitalWrite(KEYBOARD_LED, HIGH);
        } else if (!memcmp(lastKeys + 2, "801**", 5)) {
          log_d("Easter egg = 108: soft off");
          allDigitalWrite(POWER_CONTROL, HIGH);



        } else if (!memcmp(lastKeys + 2, "002**", 5)) {
          log_d("Easter egg = 200: audio test on");
          audio_test();
        } else if (!memcmp(lastKeys + 2, "102**", 5)) {
          log_d("Easter egg = 201: audio shutdown (audio test off)");
          audio->shutdown();
        } else if (!memcmp(lastKeys + 2, "202**", 5)) {
          log_d("Easter egg = 202: sending RTP stream from microphone");
          audio->openRtpConnection(5000);
          audio->sendRtpStreamFromMic(Audio::G722_RTP_PAYLOAD, IPAddress(192, 168, 1, 15), 5000);
        } else if (!memcmp(lastKeys + 2, "302**", 5)) {
          log_d("Easter egg = 203: play incoming RTP stream");
          audio->openRtpConnection(5000);
          audio->playRtpStream(Audio::G722_RTP_PAYLOAD);
        } else if (!memcmp(lastKeys + 2, "402**", 5)) {
          log_d("Easter egg = 204: recording mic audio");
          audio->setBitsPerSample(16);
          audio->setSampleRate(16000);
          audio->setMonoOutput(true);
          audio->recordFromMic();
        } else if (!memcmp(lastKeys + 2, "502**", 5)) {
          log_d("Easter egg = 205: stop recording WAV");
          char filename[100];
          sprintf(filename, "/audio_%02d%02d%02d%02d%02d%02d.pcm", ntpClock.getYear()-2000, ntpClock.getMonth(), ntpClock.getDay(), ntpClock.getHour(), ntpClock.getMinute(), ntpClock.getSecond());
          log_d("creating file %s", filename);
          audio->saveWavRecord(&SD, filename);
          audio->shutdown();



        } else if (!memcmp(lastKeys + 2, "103**", 5)) {
          log_d("Easter egg = 301: ringtone on");
          startRingtone();
        } else if (!memcmp(lastKeys + 2, "203**", 5)) {
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


    if (anyPressed) {
      msLastKeyPress = now;
      if (!keypadLedsOn) {

        allDigitalWrite(KEYBOARD_LED, LOW);
        keypadLedsOn = true;
      }
    } else if (keypadLedsOn && elapsedMillis(now, msLastKeyPress, KEYPAD_LEDS_ON_MS)) {

      allDigitalWrite(KEYBOARD_LED, HIGH);
      keypadLedsOn = false;
    }
# 2745 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {
      static uint32_t s_wifiRetryFails = 0;
      static uint32_t s_wifiQuiesceAtMs = 0;
      static bool s_prevScreenOnWifi = true;
      const bool screenOnNow = gui.state.screenBrightness > 0;
      const bool wokeNow = screenOnNow && !s_prevScreenOnWifi;
      s_prevScreenOnWifi = screenOnNow;

      if (wifiState.isConnected()) {
        s_wifiRetryFails = 0;
        s_wifiQuiesceAtMs = 0;
      }

      uint32_t retryMs = WIFI_RETRY_PERIOD_MS;
      if (s_wifiRetryFails >= 5) {
        retryMs = 180000u;
      }
      bool due = elapsedMillis(now, msLastWifiRetry, retryMs);
      if (wokeNow && !wifiState.isConnected() && !wifiState.userDisabled() &&
          (uint32_t)(now - lastWifiConnectAttemptMs()) >= 10000u) {



        due = true;
      }






      const bool xferBlocksWifi = gbcXferOn() && xferUsingAP();
      if (!xferBlocksWifi && !wifiState.scanBusy() && wifiState.doReconnect() && !wifiState.isConnected() && due && !wifiState.userDisabled()) {
        bool _cp = false;
        TIME_STEP("connectToPreferred", _cp = wifiState.connectToPreferred());
        if (_cp) {
          log_d("Connecting to WiFi");
        } else {
          log_d("Not connecting to WiFi");
        }
        msLastWifiRetry = now;
        if (s_wifiRetryFails < 1000) {
          s_wifiRetryFails++;
        }
        if (s_wifiRetryFails >= 5) {
          s_wifiQuiesceAtMs = now + 30000u;
        }
      }

      if (s_wifiQuiesceAtMs && (int32_t)(now - s_wifiQuiesceAtMs) >= 0 &&
          !wifiState.isConnected() && !wifiState.scanBusy() && !xferBlocksWifi) {





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
# 2831 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {
      CallState cs = gui.state.sipState;
      bool audioDelicate = (cs == CallState::InvitingCallee || cs == CallState::InvitedCallee ||
                            cs == CallState::RemoteRinging || cs == CallState::Call ||
                            cs == CallState::Accept || cs == CallState::BeingInvited);
      bool callBusy = audioDelicate && wifiState.isConnected();


      if (!gGbcActive && !(gbcXferOn() && xferUsingAP()) && !callBusy) {
        TIME_STEP("autoSwitchTick", wifiState.autoSwitchTick(gui.state.screenBrightness > 0));
      }
    }

#ifdef USE_VIRTUAL_KEYBOARD
    if (udpKeypad == NULL && wifiState.isConnected()) {
      log_d("Setting a connection");
      udpKeypad = new WiFiUDP();
      udpKeypad->begin(VIRTUAL_KEYBOARD_PORT);
      IPAddress ipAddr = WiFi.localIP();
      log_d("Send UDP packets to:\n%d.%d.%d.%d:%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3], VIRTUAL_KEYBOARD_PORT);
    }
#endif


    if (gui.state.msAppTimerEventPeriod > 0) {
      if (elapsedMillis(now, gui.state.msAppTimerEventLast, gui.state.msAppTimerEventPeriod)) {
        redrawWhat |= gui.processEvent(now, APP_TIMER_EVENT);
        gui.state.msAppTimerEventLast = now;
      }
    }


    EventType evnt;
    while (evnt = gui.state.popEvent(now)) {
      redrawWhat |= gui.processEvent(now, evnt);
    }


    if (updateMessageTimes) {
      gui.reloadMessages();
      updateMessageTimes = false;
    }
# 2886 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (ntpClock.isUpdated()) {
      if (waitingForClockUpdate) {
        updateMessageTimes = true;
        waitingForClockUpdate = false;
      }
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    } else if (elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS) || (ntpClock.getSecond() > 0 && elapsedMillis(now, msLastMinute, TIME_UPDATE_MINUTE_MS - ntpClock.getSecond() * 1000))) {

      ntpClock.minuteTick(now);
      msLastMinute = now;
      redrawWhat |= gui.processEvent(now, TIME_UPDATE_EVENT);
    }
# 2915 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#ifdef WIPHONE_BOARD


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
# 2956 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      gui.state.cardPresent = allDigitalRead(TF_CARD_DETECT_PIN) == LOW ? true : false;
#else
      gui.state.cardPresent = false;
#endif
# 2973 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      gui.state.battCharged = allDigitalRead(BATTERY_CHARGING_STATUS_PIN) == LOW ? true : false;
# 2987 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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






      static uint32_t s_lastCardLog = 0;
      if (s_lastCardLog == 0 || now - s_lastCardLog >= 60000u) {
        s_lastCardLog = now;
        healthLogLine(hl);






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



      log_d("Power button = %d", gpioExtender.digitalRead(POWER_CHECK & ~EXTENDER_FLAG) == LOW ? 1 : 0);
#endif
      redrawWhat |= gui.processEvent(now, BATTERY_UPDATE_EVENT);


      if (v <= 3.3 && now >= 30000) {
        powerOff();
        redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
      }
    }


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


    if (elapsedMillis(now, msLastUsbCheck, USB_CHECK_PERIOD_MS)) {
      msLastUsbCheck = now;

      bool usbHere = digitalRead(BATTERY_PPR_PIN) == LOW ? true : false;
      usbConnected += usbHere ? 1 : 0;
      usbConnectedChecks += 1;
      if (usbHere != gui.state.usbConnected) {

        gui.state.usbConnected = usbHere;


        gui.state.battBlinkOn = usbHere;
        redrawWhat |= gui.processEvent(now, USB_UPDATE_EVENT);
      } else if (usbHere && !gui.state.battCharged && gui.state.battSoc < 100) {
#ifndef BATTERY_BLINKING_OFF

        gui.state.battBlinkOn = !gui.state.battBlinkOn;
        redrawWhat |= gui.processEvent(now, BATTERY_BLINK_EVENT);
        log_v("blinked");
#endif
      }
      if (!(usbConnectedChecks & 15)) {
        log_d("USB = %d (%.1f%%), checks=%d", gui.state.usbConnected, 100.0 * usbConnected / usbConnectedChecks, usbConnectedChecks);
      }
    }


#ifdef WIPHONE_INTEGRATED_1_4
    if (powerButtonPressed && !poweringOff && elapsedMillis(now, msPowerOffStarted, 2500)) {
      powerOff();
      redrawWhat |= gui.processEvent(now, POWER_OFF_EVENT);
    }
#endif
#endif

    if (gui.state.inputCurKey && msLastKeyInput && elapsedMillis(now, msLastKeyInput, KEYPAD_IDLE_MS)) {
      log_i("keypad idle");
      redrawWhat |= gui.processEvent(now, KEYBOARD_TIMEOUT_EVENT);
      msLastKeyInput = 0;
    }


#ifdef USER_SERIAL

    while (userSerial.available() > 0) {
      char ch = userSerial.read();
      if (gGpsNmea) {


        gGpsRawBuf[gGpsRawHead] = (uint8_t)ch;
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

      log_d("User serial: %s", gui.state.userSerialBuffer.getCopy());
      redrawWhat |= gui.processEvent(now, USER_SERIAL_EVENT);
      userSerialLastSize = gui.state.userSerialBuffer.size();
    }
#endif


    if (redrawWhat & REDRAW_ALL) {
      gui.redrawScreen(redrawWhat & REDRAW_HEADER, redrawWhat & REDRAW_FOOTER, redrawWhat & REDRAW_SCREEN, redrawWhat & LOCK_UNLOCK);
    }




    if (gui.state.hasSipAccount() && !wifiState.isConnected()) {
      if (sip.isBusy()) {
        log_d("Device disconnected from WIFI");
        log_d("Call will be terminated");


        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        sip.wifiTerminateCall();




        stopRingtone();
        gui.exitCall();

        gui.state.setSipState(CallState::HungUp);
        appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        wifiTerminateSip = TERMINATE_OK;


      } else {




      }
    }


    if (rtpSilentPeriod == RTP_SILENT_ON) {

      rtpSilentPeriod = RTP_SILENT_OFF;

      if (rtpSilentCnt == 0x01) {
        rtpSilentCnt = 0x0;


        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        if (sip.isBusy()) {
          log_d("No RTP Packets From Remote Part");

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


    if (gui.state.hasSipAccount() && wifiState.isConnected() && sipMayPoll()) {
      if( wifiTerminateSip == TERMINATE_OK ) {
# 3218 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
        gui.state.setSipState(CallState::NotInited);
        wifiTerminateSip = 0x0;
      }
      if (gui.state.sipState == CallState::NotInited || gui.state.sipAccountChanged) {
        log_d("SIP is going to init");

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

          log_e("failed to connect to SIP");
          gui.state.setSipState(CallState::Error);
        }
        Random.feed(now);
        gui.state.sipEnabled = true;
        gui.state.sipAccountChanged = false;

      } else if (gui.state.sipState == CallState::Idle) {
        sip.triedToMakeCallCounter = 0;
        bool anySip = false;
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
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

          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);


          if (isRegistered != gui.state.sipRegistered) {

            gui.state.sipRegistered = isRegistered;

            res |= gui.processEvent(now, REGISTRATION_UPDATE_EVENT);



            log_e("SIP REGISTRATION -> %s", gui.state.sipRegistered ? "REGISTERED" : "lost");
          }
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);

        } else if (gui.state.outgoingMessages.size()) {


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

        bool anySip = false;
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

          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::InvitingCallee and gui.state.sipRegistered) {



        log_d("Calling: %s", gui.state.calleeUriDyn);
        if (strchr(gui.state.calleeUriDyn, '@') != NULL and strlen(gui.state.calleeUriDyn)>0 and gui.state.sipRegistered) {
          sip.startCall(gui.state.calleeUriDyn, now);

          gui.state.setSipState(CallState::InvitedCallee);
          gui.redrawScreen(true, true, true, true);
        } else {
          log_e("sip callee unavailable");
          gui.state.setSipState(CallState::Idle);
        }

      } else if (gui.state.sipState == CallState::InvitedCallee) {

        IPAddress rtpRemoteIP((uint32_t) 0);
        int rtpRemotePort = 0;
        uint16_t rtpLocalPort = 0;
        uint8_t audioFormat = TinySIP::NULL_RTP_PAYLOAD;

        bool callEstablished = false;
        bool anySip = false;
        TinySIP::StateFlags_t res;
        do {
          res = sip.checkCall(now);
          if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            anySip = true;
          }
          if (res & TinySIP::EVENT_CALL_CONFIRMED) {
            if (gui.state.sipState != CallState::Call) {
              log_d("call established");
              callEstablished = true;


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

              msHungUp = now;
            } else {
              gui.state.setSipState(CallState::Decline);
              log_d("call @ InvitedCallee is Declined");
            }
          } else if (res != TinySIP::EVENT_NONE && res != TinySIP::EVENT_RESPONSE_PARSED && res != TinySIP::EVENT_REQUEST_PARSED) {
            log_d("UNPROCESSED CALL STATE: 0x%x", res);
          }
        } while (res & TinySIP::EVENT_MORE_BUFFER);



        if (anySip) {
          log_d("setting reason @ CallState::InvitedCallee");
          gui.state.setSipReason(sip.getReason());
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }


        if (callEstablished) {

          if ((uint32_t)rtpRemoteIP && rtpRemotePort && audioFormat != TinySIP::NULL_RTP_PAYLOAD) {
            audio->openRtpConnection(rtpLocalPort);




            audio->sendRtpStreamFromMic(audioFormat, rtpRemoteIP, rtpRemotePort);
            audio->playRtpStream(audioFormat, rtpRemotePort);
          } else {
            log_e("audio session failure");
            gui.state.setSipReason("audio failed");

            appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
            gui.redrawScreen(false, false, true);
          }
        }

      } else if (gui.state.sipState == CallState::Call) {



        bool anySip = false;
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

          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangingUp) {

        bool anySip = false;
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


        if (anySip || elapsedMillis(now, msHangingUp, HANGUP_TIMEOUT_MS)) {
          if (anySip) {
            log_d("setting reason @ CallState::HangingUp");
            gui.state.setSipReason(sip.getReason());
          } else {
            log_d("hang up timeout");
            gui.state.setSipState(CallState::Idle);
            log_d("caller free (1) = %s", sip.isBusy() ? "NO" : "YES");
          }


          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }

      } else if (gui.state.sipState == CallState::HangUp) {


        log_d("Terminating call");
        stopRingtone();

        audio->showAudioStats();
        audio->shutdown();
        audio->setVolumes(restoreSpeakerVol, restoreHeadphonesVol, restoreLoudspeakerVol);

        int res = sip.terminateCall(now);
        if (res == TINY_SIP_OK) {
          msHangingUp = now;


          gui.state.setSipState(CallState::HangingUp);
        } else {
          log_d("terminating error = %d", res);
          gui.state.setSipState(CallState::HungUp);

          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        }
# 3565 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      } else if (gui.state.sipState == CallState::HungUp) {



        if (elapsedMillis(now, msHungUp, GUI::HUNGUP_TO_NORMAL_MS)) {

          log_d("hungup timeout: now = %d, msHungUp = %d", now, msHungUp);
          gui.state.setSipState(CallState::Idle);
          log_d("caller free (2) = %s", sip.isBusy() ? "NO" : "YES");
          appEventResult res = gui.processEvent(now, CALL_UPDATE_EVENT);
          gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN, true);
        }

      }


      TextMessage* msg = NULL;
      if (msg = sip.checkMessage(now, ntpClock.getExactUtcTime(), ntpClock.isTimeKnown())) {
        log_v("message received");

        gui.flash.messages.saveMessage(msg->message, msg->from, msg->to, true, msg->useTime ? msg->utcTime : 0);
        delete msg;

        appEventResult res = gui.processEvent(now, NEW_MESSAGE_EVENT);
        gui.redrawScreen(res & REDRAW_HEADER, res & REDRAW_FOOTER, res & REDRAW_SCREEN);
        notifyMessageArrived(now, gui.state.notifySipMode);
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

      MessageData* msg = gui.state.outgoingLoraMessages[0];
      if (msg) {
        lora.send_message(msg->getOtherUri(), msg->getMessageText());
        gui.flash.messages.setSent(*msg);
        delete msg;
        gui.state.outgoingLoraMessages.remove(0);
      }
    }
#endif
# 3623 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (!gGbcActive) {
      musicPlayerLoop();
    }
# 3645 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    {
      if (sipCallActive() && musicPlayerIsPlaying()) {
        musicPlayerPause();
      }
    }




    if (!gGbcActive && meshService.loop()) {
      log_d("Received Meshtastic message");
      gui.state.meshUnread = true;
      gui.processEvent(now, NEW_MESSAGE_EVENT);

      gui.redrawScreen(true, true, true);
      const MeshMessage* nm = meshService.getMessage(0);
      if (nm) {
        const MeshNode* n = meshService.findNode(nm->from);
        char title[32];
        snprintf(title, sizeof(title), "Mesh: %s", n ? n->name : "new message");
        gui.showMeshPopup(title, nm->text);
        meshPopupActive = true;
        meshPopupShownMs = now;
      }
      notifyMessageArrived(now, gui.state.notifyMeshMode);
    }






    if (!gGbcActive && meshService.takePlacesNews()) {
      gui.processEvent(now, NEW_MESSAGE_EVENT);
    }


    if (meshVibroActive && elapsedMillis(now, meshVibroStartMs, MESH_VIBRO_MS)) {
      allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
      meshVibroActive = false;
    }
# 3697 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
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


    if (meshPopupActive && elapsedMillis(now, meshPopupShownMs, MESH_POPUP_MS)) {
      meshPopupActive = false;
      gui.redrawScreen(true, true, true, true);
    }
# 3725 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    if (gui.state.ringing && gui.state.sipState != CallState::Call) {





      const bool tonePlaying = (gui.state.ringerMode == ControlState::RINGER_RING_AND_VIBRATE);



      if (tonePlaying && audio->isEof()) {


        audio->rewind();


        gui.state.vibroOn = false;
        gui.state.vibroToggledMs = now;
        gui.state.vibroNextDelayMs = gui.state.vibroDelayMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, LOW);
        allDigitalWrite(KEYBOARD_LED, HIGH);

      }



      if (gui.state.ringerMode != ControlState::RINGER_SILENT &&
          elapsedMillis(now, gui.state.vibroToggledMs, gui.state.vibroNextDelayMs)) {

        gui.state.vibroToggledMs = now;
        gui.state.vibroOn = !gui.state.vibroOn;
        gui.state.vibroNextDelayMs = gui.state.vibroOn ? gui.state.vibroOnPeriodMs : gui.state.vibroOffPeriodMs;
        allDigitalWrite(VIBRO_MOTOR_CONTROL, gui.state.vibroOn ? HIGH : LOW);
        allDigitalWrite(KEYBOARD_LED, gui.state.vibroOn ? LOW : HIGH);

      }
    }





    audio->loop();
# 3825 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    bool idleTickStretch = false;
    {
# 3866 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
#define UI_IDLE_DOWNCLOCK 0
#define UI_WORK_HOLD_MS 2000
# 3894 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
      const bool busy = (gui.state.screenBrightness > 0) ||
                        gGbcActive ||
                        gGpsNmea ||
                        xferOn() ||
                        musicPlayerIsPlaying() ||
                        sipNeedsFullSpeed();



      const bool hardBusy = gGbcActive || gGpsNmea || xferOn() || musicPlayerIsPlaying() || sipNeedsFullSpeed();
      extern volatile uint32_t gUiWorkMs;
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



        log_e("CPU %luMHz (%s)", (unsigned long)wantMhz,
#if UI_IDLE_DOWNCLOCK
              hardBusy ? "busy" : (gui.state.screenBrightness > 0 ? "screen idle" : "idle"));
#else
              busy ? "busy" : "idle");
#endif
      }




      idleTickStretch = !busy && !smsMirrorPollBusy();
    }
# 3953 "/Users/nickhowe/wiphone/WiPhone/WiPhone.ino"
    vTaskDelay(idleTickStretch ? 5 : 1);




  }
}

void powerOff() {
  log_i("POWER OFF");
  allDigitalWrite(POWER_CONTROL, HIGH);
  msPowerOffStarted = millis();
  poweringOff = true;
}