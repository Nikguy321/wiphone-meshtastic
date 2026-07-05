/*
 * meshtastic_service.h — WiPhone Meshtastic integration (Phase 3 shell)
 *
 * Background service that owns the mesh radio, an in-RAM store of recent
 * messages and a small node database, and exposes a simple non-blocking API
 * to the WiPhone UI (see app_meshtastic.*).
 *
 * In this first patch the radio layer is STUBBED: setup()/loop() do not touch
 * hardware yet, and send*() echo locally. The real Meshtastic SX1276 PHY
 * (sync word 0x2b, LongFast preset, AES-CTR, protobuf) will be added behind
 * this same API in a later patch without changing the UI.
 */

#ifndef MESHTASTIC_SERVICE_H
#define MESHTASTIC_SERVICE_H

#include <Arduino.h>
#include <stdint.h>

#define MESH_MSG_CAP        1000     // total message capacity (allocated in PSRAM)
#define MESH_MAX_PER_CHAT    150     // max stored messages per conversation
#define MESH_MAX_NODES       32      // known-node database size
#define MESH_TEXT_LEN       234      // max text payload (Meshtastic on-air max ~233)
#define MESH_NAME_LEN        24      // node long/short name buffer
#define MESH_MAX_CHANNELS     8      // channels we can monitor at once
#define MESH_KEY_LEN         32      // max channel key length (AES-256)

#define MESH_BROADCAST_ADDR  0x00000000u   // "to" value for a channel broadcast

typedef enum {
  MESH_RADIO_UNINITIALIZED = 0,
  MESH_RADIO_STUBBED,               // service running, no real PHY yet
  MESH_RADIO_READY,                 // real radio initialized (future)
  MESH_RADIO_ERROR,
} MeshRadioState;

// MeshMessage.flags bits (kept in one byte so the struct size is stable).
#define MESH_MSG_OUTGOING  0x01     // sent from this device
#define MESH_MSG_READ      0x02     // the user has viewed this message's thread

typedef struct {
  uint32_t from;                    // sender node number
  uint32_t to;                      // MESH_BROADCAST_ADDR for channel, else dest node
  uint32_t timeMs;                  // millis() when stored (real RX time added later)
  uint8_t  channelHash;             // channel this message belongs to
  uint8_t  flags;                   // MESH_MSG_OUTGOING | MESH_MSG_READ
  char     text[MESH_TEXT_LEN];
} MeshMessage;

typedef struct {
  uint32_t nodeNum;                 // node number (0 = empty slot)
  char     name[MESH_NAME_LEN];     // display name
  uint32_t lastHeardMs;             // millis() of last packet (0 = unknown)
  int16_t  snr;                     // last SNR (x1, stub for now)
} MeshNode;

typedef struct {
  char    name[MESH_NAME_LEN];      // channel name (e.g. "LongFast", "Howe group")
  uint8_t key[MESH_KEY_LEN];        // channel key (AES-128 or AES-256)
  uint8_t keyLen;                   // 16 or 32
  uint8_t hash;                     // channel hash (name ^ key), matches packet header
} MeshChannel;

class MeshtasticService {
public:
  MeshtasticService();

  // Lifecycle (called from the WiPhone superloop)
  void setup();                     // init identity + stub radio; safe if called once
  bool loop();                      // non-blocking tick; returns true if a new
                                    // inbound message was stored this call

  // ---- UI-facing API -------------------------------------------------------
  // Messages are ordered newest-first: index 0 is the most recent.
  int                getMessageCount() const;
  const MeshMessage* getMessage(int index) const;   // NULL if out of range

  int                getUnreadTotal() const;        // count of unread messages
  // Mark a conversation's messages read (channel by hash, or DM by peer).
  void               markRead(bool isChannel, uint8_t channelHash, uint32_t peer);

  // Nodes are ordered most-recently-heard first.
  int                getNodeCount() const;
  const MeshNode*    getNode(int index) const;      // NULL if out of range
  const MeshNode*    findNode(uint32_t nodeNum) const;

  // ---- Channels ------------------------------------------------------------
  int                getChannelCount() const;
  const MeshChannel* getChannel(int index) const;
  const MeshChannel* findChannelByHash(uint8_t hash) const;
  // Parse a Meshtastic channel URL (https://meshtastic.org/e/#... or the raw
  // base64 fragment) and merge its channels in. Returns # of channels added.
  int  applyChannelUrl(const char* url);

  // Send a broadcast on a specific channel (by hash), or a DM (primary channel).
  bool sendChannelMessage(uint8_t channelHash, const char* text);
  bool sendDirectMessage(uint32_t destNode, const char* text);

  // ---- This node's identity ------------------------------------------------
  const char* getMyLongName()  const { return myLongName; }
  const char* getMyShortName() const { return myShortName; }
  void setMyName(const char* longName);      // persist (NVS) + re-announce
  void announceNodeInfo(bool wantResponse);  // broadcast our NodeInfo now

  // Hop limit for packets we originate (1..7). Persisted in NVS.
  uint8_t getHopLimit() const { return myHopLimit; }
  void    setHopLimit(uint8_t hops);

  // Maintenance (persisted immediately).
  void clearMessages();                      // wipe all stored messages
  void clearNodes();                         // wipe node DB (keeps this node)

  // ---- Status --------------------------------------------------------------
  MeshRadioState getRadioState() const { return radioState; }
  const char*    getRegion()     const { return region; }
  const char*    getChannelName()const { return channelName; }
  const char*    getModemPreset()const { return modemPreset; }
  uint32_t       getMyNodeNum()  const { return myNodeNum; }

private:
  void      storeMessage(uint32_t from, uint32_t to, uint8_t channelHash, const char* text, bool outgoing);
  MeshNode* upsertNode(uint32_t nodeNum, const char* name);
  void      seedStubData();         // demo node + welcome message (stub only)
  bool      seenPacketId(uint32_t id);   // dedup rebroadcasts of the same packet
  void      saveDb();               // persist nodes + messages to SPIFFS
  void      loadDb();               // restore nodes + messages from SPIFFS
  uint32_t  chatKeyOf(uint32_t from, uint32_t to) const;   // conversation id (0 = broadcast)
  void      removeMessageAt(int idx);

  // Channels
  void      initDefaultChannel();   // channel 0 = LongFast
  bool      addChannel(const char* name, const uint8_t* key, uint8_t keyLen);   // merge
  void      loadChannels();         // from SPIFFS
  void      saveChannels();         // to SPIFFS
  MeshChannel channels[MESH_MAX_CHANNELS];
  int         channelCount;

  // Messages, oldest at [0] and newest at [msgCount-1]. Allocated in PSRAM
  // (capacity MESH_MSG_CAP); capped at MESH_MAX_PER_CHAT per conversation.
  MeshMessage* messages;
  int          msgCount;

  MeshNode    nodes[MESH_MAX_NODES];
  int         nodeCount;

  MeshRadioState radioState;
  const char*    region;
  const char*    channelName;
  const char*    modemPreset;
  uint32_t       myNodeNum;
  char           myLongName[MESH_NAME_LEN];   // editable, persisted in NVS
  char           myShortName[8];              // up to 4 chars (Meshtastic)
  uint8_t        myHopLimit;                  // hop limit for originated packets

  void loadMyName();                          // load from NVS or derive default
  void deriveShortName();                     // short = first chars of long

  // Flood-routing rebroadcast queue (CLIENT role: relay others' packets).
  struct MeshPendingTx {
    uint8_t  data[MESH_KEY_LEN + 8 + MESH_TEXT_LEN + 16];
    uint8_t  len;
    uint32_t dueMs;
    bool     active;
  };
  MeshPendingTx rebroadcast[4];
  void scheduleRebroadcast(const uint8_t* pkt, uint8_t len);

  // Recently-seen packet ids, to drop mesh rebroadcasts of the same packet.
  uint32_t recentPktIds[16];
  int      recentPktPos;

  bool     dbDirty;                 // unsaved changes pending
  uint32_t lastSaveMs;              // last persistence write (millis)

  bool     initialized;
};

// Single global instance (defined in meshtastic_service.cpp).
extern MeshtasticService meshService;

#endif // MESHTASTIC_SERVICE_H
