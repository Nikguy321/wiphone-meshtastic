/*
 * mesh_crypto.h — Meshtastic payload crypto + channel hashing
 *
 * Meshtastic encrypts the packet payload (everything after the 16-byte
 * PacketHeader) with AES-CTR using the channel key (128- or 256-bit). The CTR
 * nonce is built from the packet id and sender node number.
 */

#ifndef MESH_CRYPTO_H
#define MESH_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

// XOR-hash of a byte range (used to build the channel hash).
uint8_t meshXorHash(const uint8_t* data, size_t len);

// Channel hash byte for a (name, key): xorHash(name) ^ xorHash(key).
uint8_t meshChannelHash(const char* name, const uint8_t* key, size_t keyLen);

// The well-known default channel key (16 bytes) and the LongFast channel hash.
const uint8_t* meshDefaultKey();
uint8_t        meshDefaultChannelHash();

// AES-CTR transform (encrypt == decrypt). keyLen must be 16 or 32 bytes.
bool meshCryptCtr(const uint8_t* key, size_t keyLen,
                  uint32_t fromNode, uint32_t packetId,
                  const uint8_t* in, size_t len, uint8_t* out);

#endif // MESH_CRYPTO_H
