/*
 * mesh_phy.cpp — Meshtastic-compatible SX1276 LoRa PHY (Phase 2, step 1)
 */

#include "mesh_phy.h"
#include "Hardware.h"          // RFM95_CS, RFM95_INT, HSPI_MISO/MOSI/SCLK
#include <RHSoftwareSPI.h>     // RadioHead bit-banged SPI transport

// ---- SX1276 register map (LoRa mode) --------------------------------------
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42
#define REG_PA_DAC               0x4D

// OP_MODE bits
#define MODE_LONG_RANGE_MODE     0x80    // LoRa (must set in SLEEP)
#define MODE_SLEEP               0x00
#define MODE_STDBY               0x01
#define MODE_TX                  0x03
#define MODE_RX_CONTINUOUS       0x05

// IRQ_FLAGS bits
#define IRQ_TX_DONE_MASK         0x08
#define IRQ_PAYLOAD_CRC_ERROR    0x20
#define IRQ_RX_DONE_MASK         0x40

// PA config
#define PA_BOOST                 0x80

#define SX1276_VERSION           0x12

MeshPhy meshPhy;

MeshPhy::MeshPhy()
  : spi(NULL), freqHz(MESH_PHY_FREQ_HZ), ready(false), inRx(false) {}

// ---- Low-level SPI (manual CS on RFM95_CS) --------------------------------

uint8_t MeshPhy::readReg(uint8_t addr) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(addr & 0x7f);            // MSB=0 → read
  uint8_t val = spi->transfer(0x00);
  digitalWrite(RFM95_CS, HIGH);
  return val;
}

void MeshPhy::writeReg(uint8_t addr, uint8_t val) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(addr | 0x80);            // MSB=1 → write
  spi->transfer(val);
  digitalWrite(RFM95_CS, HIGH);
}

void MeshPhy::readFifo(uint8_t* buf, uint8_t len) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(REG_FIFO & 0x7f);
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = spi->transfer(0x00);
  }
  digitalWrite(RFM95_CS, HIGH);
}

void MeshPhy::writeFifo(const uint8_t* buf, uint8_t len) {
  digitalWrite(RFM95_CS, LOW);
  spi->transfer(REG_FIFO | 0x80);
  for (uint8_t i = 0; i < len; i++) {
    spi->transfer(buf[i]);
  }
  digitalWrite(RFM95_CS, HIGH);
}

// ---- Mode / config --------------------------------------------------------

void MeshPhy::setModeIdle() {
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
  inRx = false;
}

void MeshPhy::setModeRxContinuous() {
  writeReg(REG_DIO_MAPPING_1, 0x00);     // DIO0 = RxDone (unused; we poll)
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
  inRx = true;
}

void MeshPhy::setFrequency(uint32_t hz) {
  freqHz = hz;
  // Frf = freq * 2^19 / 32MHz
  uint64_t frf = ((uint64_t)hz << 19) / 32000000ULL;
  writeReg(REG_FRF_MSB, (uint8_t)(frf >> 16));
  writeReg(REG_FRF_MID, (uint8_t)(frf >> 8));
  writeReg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

void MeshPhy::configureLongFast() {
  // MODEM_CONFIG_1: BW=250kHz (0b1000<<4), CR=4/5 (0b001<<1), explicit header (0)
  writeReg(REG_MODEM_CONFIG_1, (0x08 << 4) | (0x01 << 1) | 0x00);   // 0x82
  // MODEM_CONFIG_2: SF=11 (0xB0), RxPayloadCrcOn=1 (0x04)
  writeReg(REG_MODEM_CONFIG_2, (11 << 4) | 0x04);                   // 0xB4
  // MODEM_CONFIG_3: AGC auto on (0x04); LowDataRateOptimize off (symbol < 16ms)
  writeReg(REG_MODEM_CONFIG_3, 0x04);
  // Preamble length = 16 symbols (Meshtastic default)
  writeReg(REG_PREAMBLE_MSB, 0x00);
  writeReg(REG_PREAMBLE_LSB, 0x10);
  // Meshtastic sync word
  writeReg(REG_SYNC_WORD, MESH_PHY_SYNC_WORD);
}

// ---- Public ---------------------------------------------------------------

bool MeshPhy::begin() {
  pinMode(RFM95_CS, OUTPUT);
  digitalWrite(RFM95_CS, HIGH);

  spi = new RHSoftwareSPI();
  spi->setPins(HSPI_MISO, HSPI_MOSI, HSPI_SCLK);
  spi->begin();

  // RST is not wired on this board (RFM95_RST == -1); rely on register config.

  // Probe the chip: must be in SLEEP to switch to LoRa mode.
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
  delay(10);
  uint8_t ver = readReg(REG_VERSION);
  if (ver != SX1276_VERSION) {
    log_e("MeshPhy: SX1276 not found (REG_VERSION=0x%02X, expected 0x12)", ver);
    ready = false;
    return false;
  }
  log_i("MeshPhy: SX1276 detected (version 0x%02X)", ver);

  setModeIdle();
  setFrequency(freqHz);
  configureLongFast();

  // FIFO base addresses: give full 256-byte FIFO to both TX and RX.
  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  writeReg(REG_FIFO_RX_BASE_ADDR, 0x00);

  // PA: PA_BOOST, ~17 dBm (bring-up level; refined later).
  writeReg(REG_PA_CONFIG, PA_BOOST | 0x0F);
  writeReg(REG_PA_DAC, 0x84);

  setModeRxContinuous();
  ready = true;
  log_i("MeshPhy: LongFast RX @ %lu Hz, sync 0x%02X", (unsigned long)freqHz, MESH_PHY_SYNC_WORD);
  return true;
}

bool MeshPhy::poll(uint8_t* buf, uint16_t maxLen, uint8_t* outLen, int16_t* rssi, int8_t* snr) {
  if (!ready) {
    return false;
  }
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  if (!(irq & IRQ_RX_DONE_MASK)) {
    return false;                        // nothing received
  }
  // Clear all IRQ flags (write-1-to-clear).
  writeReg(REG_IRQ_FLAGS, 0xFF);

  if (irq & IRQ_PAYLOAD_CRC_ERROR) {
    log_w("MeshPhy: RX CRC error, dropping");
    return false;
  }

  uint16_t len = readReg(REG_RX_NB_BYTES);
  if (len > maxLen) {
    len = maxLen;
  }
  writeReg(REG_FIFO_ADDR_PTR, readReg(REG_FIFO_RX_CURRENT_ADDR));
  readFifo(buf, (uint8_t)len);

  if (outLen) {
    *outLen = len;
  }
  if (snr) {
    *snr = (int8_t)readReg(REG_PKT_SNR_VALUE) / 4;
  }
  if (rssi) {
    // LoRa RSSI (HF port): -157 + raw
    *rssi = -157 + (int16_t)readReg(REG_PKT_RSSI_VALUE);
  }
  return true;
}

bool MeshPhy::send(const uint8_t* data, uint8_t len) {
  if (!ready || !data || len == 0) {
    return false;
  }
  setModeIdle();
  writeReg(REG_FIFO_ADDR_PTR, 0x00);
  writeFifo(data, len);
  writeReg(REG_PAYLOAD_LENGTH, len);
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);

  // Poll TxDone with a timeout (small payloads finish in a few hundred ms).
  uint32_t start = millis();
  while (!(readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK)) {
    if (millis() - start > 2000) {
      log_e("MeshPhy: TX timeout");
      writeReg(REG_IRQ_FLAGS, 0xFF);
      setModeRxContinuous();
      return false;
    }
    delay(1);
  }
  writeReg(REG_IRQ_FLAGS, 0xFF);
  setModeRxContinuous();
  return true;
}
