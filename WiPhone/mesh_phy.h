/*
 * mesh_phy.h — Meshtastic-compatible SX1276 LoRa PHY (Phase 2, step 1)
 *
 * A minimal, register-level driver for the RFM95W (Semtech SX1276) that
 * configures the radio to match Meshtastic's on-air PHY: LoRa mode, the
 * LongFast modem preset (BW 250 kHz, SF 11, CR 4/5), sync word 0x2b, and the
 * US channel frequency, then runs RX-continuous. IRQs are polled (no ISR) so
 * the WiPhone superloop stays non-blocking.
 *
 * Step 1 only exposes raw packet RX/TX. Meshtastic PacketHeader parsing,
 * AES-CTR and protobuf are layered on top in later steps (in the service).
 *
 * Reuses RadioHead's RHSoftwareSPI purely as the SPI transport, because that
 * is the exact bit-banged SPI path already proven to work on this board
 * (see lora.cpp). All framing/registers are handled here directly.
 */

#ifndef MESH_PHY_H
#define MESH_PHY_H

#include <Arduino.h>
#include <stdint.h>

// Meshtastic LoRa payload cap (protobuf-encoded MeshPacket over the air).
#define MESH_PHY_MAX_PAYLOAD  256

// LongFast US default primary-channel frequency, in Hz.
// Derived from Meshtastic's US region (freqStart 902.0 MHz, BW 250 kHz):
//   freq = 902.0 + BW/2 + channel_num * BW  → 906.875 MHz for the LongFast slot.
// Overridable so we can retune against a real node during bring-up.
#ifndef MESH_PHY_FREQ_HZ
#define MESH_PHY_FREQ_HZ  906875000UL
#endif

// Meshtastic LoRa sync word (distinguishes the mesh from generic LoRa).
#define MESH_PHY_SYNC_WORD  0x2b

class RHSoftwareSPI;   // forward decl (RadioHead transport)

class MeshPhy {
public:
  MeshPhy();

  // Initialize SPI + radio. Returns true if the SX1276 is detected
  // (REG_VERSION == 0x12) and configured. Non-fatal on failure (returns false).
  bool begin();

  bool isReady() const { return ready; }

  // Non-blocking RX poll. If a full LoRa packet is available, copies up to
  // `maxLen` bytes into `buf`, sets *outLen, fills RSSI/SNR, and returns true.
  // Otherwise returns false immediately.
  bool poll(uint8_t* buf, uint16_t maxLen, uint8_t* outLen, int16_t* rssi, int8_t* snr);

  // Transmit a raw LoRa payload (blocks until TxDone or timeout, then returns
  // to RX-continuous). Used by later steps for Meshtastic TX.
  bool send(const uint8_t* data, uint8_t len);

  uint32_t getFrequencyHz() const { return freqHz; }

private:
  uint8_t readReg(uint8_t addr);
  void    writeReg(uint8_t addr, uint8_t val);
  void    readFifo(uint8_t* buf, uint8_t len);
  void    writeFifo(const uint8_t* buf, uint8_t len);

  void setModeIdle();
  void setModeRxContinuous();
  void setFrequency(uint32_t hz);
  void configureLongFast();

  RHSoftwareSPI* spi;
  uint32_t       freqHz;
  bool           ready;
  bool           inRx;
};

extern MeshPhy meshPhy;

#endif // MESH_PHY_H
