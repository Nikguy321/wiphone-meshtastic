/*
 * mesh_crypto.cpp — Meshtastic payload crypto + channel hashing
 */

#include "mesh_crypto.h"
#include <string.h>
#include "mbedtls/aes.h"

// The well-known Meshtastic default channel key (AES-128). A channel whose PSK
// is the single byte {0x01} uses exactly this key ("default" / LongFast).
static const uint8_t MESH_DEFAULT_KEY[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
  0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static const char MESH_DEFAULT_CHANNEL_NAME[] = "LongFast";

uint8_t meshXorHash(const uint8_t* data, size_t len) {
  uint8_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
  }
  return h;
}

// Channel hash byte = xorHash(name) ^ xorHash(key). Meshtastic uses this both
// for channel matching and to select which channel a packet belongs to.
uint8_t meshChannelHash(const char* name, const uint8_t* key, size_t keyLen) {
  uint8_t nameHash = meshXorHash((const uint8_t*)name, name ? strlen(name) : 0);
  uint8_t keyHash = meshXorHash(key, keyLen);
  return nameHash ^ keyHash;
}

const uint8_t* meshDefaultKey() {
  return MESH_DEFAULT_KEY;
}

uint8_t meshDefaultChannelHash() {
  return meshChannelHash(MESH_DEFAULT_CHANNEL_NAME, MESH_DEFAULT_KEY, sizeof(MESH_DEFAULT_KEY));
}

// AES-CTR transform (encrypt == decrypt). Supports 16-byte (AES-128) and
// 32-byte (AES-256) keys. Meshtastic CTR nonce (16 bytes): packet id as
// little-endian u64 in bytes 0..7, sender node as little-endian u32 in 8..11.
bool meshCryptCtr(const uint8_t* key, size_t keyLen,
                  uint32_t fromNode, uint32_t packetId,
                  const uint8_t* in, size_t len, uint8_t* out) {
  if (!key || !in || !out || len == 0) {
    return false;
  }
  if (keyLen != 16 && keyLen != 32) {
    return false;
  }

  uint8_t nonce[16];
  memset(nonce, 0, sizeof(nonce));
  nonce[0] = (uint8_t)(packetId >> 0);
  nonce[1] = (uint8_t)(packetId >> 8);
  nonce[2] = (uint8_t)(packetId >> 16);
  nonce[3] = (uint8_t)(packetId >> 24);
  nonce[8]  = (uint8_t)(fromNode >> 0);
  nonce[9]  = (uint8_t)(fromNode >> 8);
  nonce[10] = (uint8_t)(fromNode >> 16);
  nonce[11] = (uint8_t)(fromNode >> 24);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, (unsigned)keyLen * 8) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }

  size_t  ncOff = 0;
  uint8_t streamBlock[16];
  memset(streamBlock, 0, sizeof(streamBlock));
  int rc = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, streamBlock, in, out);
  mbedtls_aes_free(&ctx);
  return rc == 0;
}
