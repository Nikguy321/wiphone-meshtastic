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
#define MESH_SHORT_NAME_MAX   4      // Meshtastic short name: 4 characters, by convention

/* How often we re-announce our own NodeInfo. Matches stock firmware's
 * node_info_broadcast_secs default of 3 hours, so this phone behaves like the radios
 * around it. Discovery is passive: without this, anything that boots or clears its node
 * DB after us never learns our name. */
#define MESH_NODEINFO_PERIOD_MS     (3UL * 60UL * 60UL * 1000UL)
/* Minimum gap between NodeInfo replies to want_response requests. One "ask everyone to
 * announce" reaches every node at once; answering instantly every time collides on air and
 * wastes the channel. Real firmware damps these deliberately. */
#define MESH_NODEINFO_REPLY_MIN_MS  (60UL * 1000UL)
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

// MeshNode.pkiFlags bits.
#define MESH_NODE_HAS_KEY       0x01   // pubKey holds this node's X25519 key
#define MESH_NODE_KEY_MISMATCH  0x02   // a LATER NodeInfo carried a DIFFERENT key
                                       // (kept the first one — trust-on-first-use,
                                       // like stock; Clear nodes re-learns)

typedef struct {
  uint32_t nodeNum;                 // node number (0 = empty slot)
  char     name[MESH_NAME_LEN];     // display name
  uint32_t lastHeardMs;             // millis() of last packet (0 = unknown)
  int16_t  snr;                     // last SNR (x1, stub for now)
  /* PKC: the node's X25519 public key, learned from its NodeInfo (User field 8).
   * Appended at the END so existing readers keep their offsets; saveDb/loadDb
   * migrate the v1 on-flash layout. The derived AES key is deliberately NOT
   * stored here: it lives in a tiny LRU (see pkiCache) so 32 nodes don't cost
   * another kilobyte of internal RAM — RAM is what panics this phone. */
  uint8_t  pubKey[MESH_KEY_LEN];
  uint8_t  pkiFlags;                // MESH_NODE_HAS_KEY | MESH_NODE_KEY_MISMATCH
  /* Last heard position (port 3 broadcasts — COVEY sends one every 5 min).
   * The phone has no GPS: it only listens. posHeardMs==0 means never heard.
   * Appended after the v2 fields; DB migrates v1/v2 in place. */
  int32_t  latI, lonI;              // 1e-7 degrees
  uint32_t posHeardMs;              // millis() when the position arrived
} MeshNode;

// A waypoint shared over the mesh (port 8) — camp, the truck, a stand. The
// reference point for every distance the UI shows.
typedef struct {
  uint32_t id;                      // 0 = empty slot
  int32_t  latI, lonI;
  uint32_t expire;                  // unix seconds, 0 = never
  uint32_t lockedTo;                // owner node: only it may change/expire this (0 = anyone)
  uint32_t heardMs;                 // millis() of last update (0 after a reboot restore)
  char     name[20];                // MESH_WP_NAME_LEN
} MeshWaypoint;

#define MESH_MAX_WAYPOINTS  8

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

  // ---- Neighbour info (portnum 71) -----------------------------------------
  /* Who we hear DIRECTLY, and (optionally) telling the mesh about it so a
   * who-hears-whom map can be drawn. Off by default: it is airtime. */
  uint32_t getNeighborInterval() const { return nbrIntervalSecs; }   // 0 = off
  void     setNeighborInterval(uint32_t secs);                       // persists (NVS)
  int      getDirectNeighborCount() const { return nbrCount; }
  bool     getDirectNeighbor(int i, uint32_t* node, int* snr, uint32_t* ageMs) const;
  uint32_t lastNeighborTxMs() const { return nbrLastTxMs; }
  int      lastNeighborTxCount() const { return nbrLastTxCount; }
  const char* neighborChannelName() const;   // NULL = no private channel to send on
  int      announceNeighborsNow();           // announce on every private channel; count sent

  // ---- Mesh history replay (docs/replay-spec.md) ---------------------------
  // This phone remembers the channel texts it hears; COVEY, blind while its
  // radio is lent, asks `RPL?` on the booksync channel and gets them back.
  int      replayRingCount() const   { return replayCount; }
  int      replayPendingPackets() const;
  uint32_t replayLastServedMs() const { return replayServedMs; }
  int      replayLastServedN() const  { return replayServedN; }

  // ---- This node's identity ------------------------------------------------
  const char* getMyLongName()  const { return myLongName; }
  const char* getMyShortName() const { return myShortName; }
  void setMyName(const char* longName);      // persist (NVS) + re-announce
  /* Set the 4-char short name other radios show. Persisted, and it STOPS the short name
   * following the long one. Pass NULL or "" to go back to deriving it. */
  void setMyShortName(const char* shortName);
  bool isShortNameCustom() const { return shortNameCustom; }
  void announceNodeInfo(bool wantResponse);  // broadcast our NodeInfo now

  // Hop limit for packets we originate (1..7). Persisted in NVS.
  uint8_t getHopLimit() const { return myHopLimit; }
  void    setHopLimit(uint8_t hops);

  // ---- PKC (Meshtastic 2.5+ DM crypto — see mesh_pki.h) --------------------
  // Our X25519 identity. Generated once, persisted in NVS; announced in every
  // NodeInfo so 2.5+ nodes will DM us (they refuse without it).
  bool           pkiIsReady()   const { return pkiReady; }
  const uint8_t* pkiPublicKey() const { return myPkiPub; }

  // ---- Positions & places (see mesh_pos.h) ---------------------------------
  int                 getWaypointCount() const;
  const MeshWaypoint* getWaypoint(int index) const;       // NULL if out of range
  const MeshWaypoint* findWaypoint(uint32_t id) const;

  /* The phone's own position is a USER-DECLARED pin ("I'm at camp"), not a fix
   * — there is no GPS. Setting it persists to NVS and broadcasts one Position
   * so COVEY's map shows this phone. pinnedAtUnix is 0 when the clock was
   * unknown at pin time. setMyPin returns the ANNOUNCE result: false means the
   * pin stuck locally but never went on air — the UI must not imply otherwise. */
  bool getMyPin(int32_t* latI, int32_t* lonI, uint32_t* pinnedAtUnix) const;
  bool setMyPin(int32_t latI, int32_t lonI);              // persist + announce
  void clearMyPin();
  bool pinAnnounceOk() const { return lastAnnounceOk; }   // did the last announce transmit?

  /* Latched "positions or places changed" signal for the UI — read-and-clear.
   * Separate from loop()'s new-MESSAGE return on purpose: a waypoint arriving
   * must refresh an open Places screen but must NOT buzz, pop up, or light the
   * unread icon. (The book-sync 'receiver shows nothing' lesson, D-089.) */
  bool takePlacesNews() { bool n = placesNews; placesNews = false; return n; }

  /* Which place distances are measured FROM: a waypoint id, or 0 = automatic
   * (a live GPS fix when one is fresh, else the pin). Persisted.
   * resolveReference() gives the coords + a display name, false if none exist. */
  uint32_t getReferenceId() const { return refWaypointId; }
  void     setReferenceId(uint32_t id);
  bool     resolveReference(int32_t* latI, int32_t* lonI,
                            char* name, size_t nameCap) const;

  /* ---- The woods backplate's GPS (nmea.h) ----------------------------------
   * A LIVE fix (fresh within 2 min) slots into resolveReference between the
   * chosen waypoint and the manual pin: "where am I NOW" beats a pin set hours
   * ago, but an explicit waypoint choice still beats both. It NEVER touches
   * announce — putting this phone's position on the air stays a deliberate act
   * (the COVEY public-LongFast lesson). */
  void gpsUpdate(bool valid, int32_t latI, int32_t lonI, int sats, int hdopX10);
  bool getGpsFix(int32_t* latI, int32_t* lonI, uint32_t* ageMs,
                 int* sats, int* hdopX10) const;
  bool gpsEverFixed() const { return gpsFixMs != 0; }

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
  bool           shortNameCustom;             // user set it: do NOT re-derive from long
  uint8_t        myHopLimit;                  // hop limit for originated packets

  /* NodeInfo beaconing. Discovery on a LoRa mesh is PASSIVE — a node is learned only when
   * it transmits — so a node that never re-announces is invisible to anything that arrives
   * after it. See the periodic block in loop(). */
  uint32_t       nextNodeInfoMs;              // when the next periodic announce is due
  uint32_t       lastNodeInfoTxMs;            // damps replies to want_response requests

  // ---- PKC state -----------------------------------------------------------
  uint8_t  myPkiPriv[MESH_KEY_LEN];           // X25519 private key (NVS "pkipriv")
  uint8_t  myPkiPub[MESH_KEY_LEN];            // matching public key (announced)
  bool     pkiReady;                          // keypair loaded/generated OK

  /* Derived-AES-key cache, 2 entries. One X25519 derive costs ~3 KB of
   * TRANSIENT stack and tens of ms, so it is only ever run at SUPERLOOP depth
   * (RX key-learn, boot pre-warm, the pending-DM drain in loop()) — never from
   * GUI code. sendDirectMessage at GUI depth uses cache hits only and QUEUES
   * on a miss. Two entries cover the real fleet (COVEY + one more). */
  struct PkiCacheEntry {
    uint32_t node;                            // 0 = empty
    uint8_t  key[MESH_KEY_LEN];               // SHA-256(X25519 shared secret)
  };
  PkiCacheEntry pkiCache[2];
  uint8_t       pkiCacheNext;                 // round-robin victim
  void loadOrCreatePkiKeys();                 // NVS load, or generate + store
  bool pkiKeyForNode(const MeshNode* n, uint8_t keyOut[MESH_KEY_LEN]);  // derive (superloop only!)
  bool pkiKeyCached(uint32_t nodeNum, uint8_t keyOut[MESH_KEY_LEN]) const;
  void pkiCacheClear() { memset(pkiCache, 0, sizeof(pkiCache)); pkiCacheNext = 0; }
  void pkiLearnKey(MeshNode* n, const uint8_t* key);   // TOFU store + eager derive

  /* DMs queued because their AES key wasn't cached at send time (GUI depth).
   * Drained by loop() one per tick: derive there, send, local-echo. */
  struct PendingDm {
    uint32_t dest;
    char     text[MESH_TEXT_LEN];
    bool     active;
  };
  PendingDm pendingDm[2];

  // Packet ids of DMs we stored and ACKed: a retransmission of one of these is
  // re-ACKed (our ACK may have been lost) but never re-stored.
  uint32_t recentAckIds[8];
  uint8_t  recentAckPos;

  // ---- Positions & places state --------------------------------------------
  MeshWaypoint waypoints[MESH_MAX_WAYPOINTS];
  int          waypointCount;
  int32_t      myPinLatI, myPinLonI;      // 0/0 + myPinSet=false = no pin
  bool         myPinSet;
  uint32_t     myPinAtUnix;               // when pinned (0 = clock was unknown)
  uint32_t     refWaypointId;             // 0 = use the pin
  bool         placesNews;                // latched: positions/waypoints changed
  bool         lastAnnounceOk;            // last announceMyPosition() transmitted
  uint32_t     nextWpSweepMs;             // next local waypoint-expiry sweep
  int32_t      gpsLatI, gpsLonI;          // last VALID GPS fix (1e-7 deg)
  uint32_t     gpsFixMs;                  // millis() of that fix; 0 = never
  int          gpsSats, gpsHdopX10;       // -1 = unknown
  void upsertWaypoint(uint32_t id, int32_t latI, int32_t lonI,
                      uint32_t expire, uint32_t lockedTo, const char* name);
  void loadPin();                          // NVS
  bool announceMyPosition();               // one Position broadcast (private ch preferred)

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

  // ---- Neighbour info state -------------------------------------------------
  /* DELIBERATELY NOT PERSISTED and deliberately not part of MeshNode: who is
   * within direct earshot is a LIVE property (it changes as you walk), and
   * adding a field to MeshNode would force a node-DB migration for data that
   * is stale the moment it is restored. */
  struct DirectNeighbor {
    uint32_t nodeNum;
    uint32_t heardMs;
    int8_t   snr;
  };
  DirectNeighbor nbr[16];
  int            nbrCount;
  uint32_t       nbrIntervalSecs;    // 0 = off (NVS "nbrint")
  uint32_t       nbrNextTxMs;
  uint8_t        nbrPendingMask;    // channels still to announce this cycle (bit = index)
  uint32_t       nbrDripMs;         // next send in the current cycle
  uint32_t       nbrLastTxMs;        // millis() of the last announce (0 = never)
  int            nbrLastTxCount;     // neighbours in it
  void neighborHeard(uint32_t node, int8_t snr, bool direct);
  bool announceNeighborsOn(const MeshChannel* ch);
  const MeshChannel* neighborChannel() const;
  bool channelIsMachine(const MeshChannel* ch) const;

  // ---- Mesh history replay state (docs/replay-spec.md) ---------------------
  /* The ring lives in PSRAM (setup(); ~12 KB) — NOT internal heap, which this
   * phone has none of to spare. NULL = allocation failed = feature inert. */
  struct ReplayHeard {
    uint32_t rxUnix;                    // WiPhone clock when heard (never 0 in ring)
    uint32_t sender;
    char     chan[16];                  // channel NAME (names travel; indexes differ)
    char     text[160];
  };
  ReplayHeard* replayRing;
  int          replayHead;              // next write slot
  int          replayCount;
  /* Reply packets, built at request time, dripped one per REPLAY_TX_GAP_MS —
   * LoRa airtime politeness. A new request restarts the queue (latest wins). */
  char*        replayPkts;              // PSRAM slab: REPLAY_MAX_PKTS × REPLAY_PKT_STRIDE
  int          replayPktCount;
  int          replayPktNext;
  uint32_t     replayNextTxMs;
  uint8_t      replayChanHash;          // the channel the request arrived on
  uint32_t     replayCoverFrom;         // unix secs this ring's coverage began (0 = none yet)
  uint32_t     replayServedMs;          // millis() of the last served request (0 = never)
  int          replayServedN;           // records in that reply
  void replayCapture(uint32_t sender, const MeshChannel* ch, const char* text);
  void replayHandleRequest(const MeshChannel* ch, const char* text);
  void replayPump();

  bool     dbDirty;                 // unsaved changes pending
  uint32_t lastSaveMs;              // last persistence write (millis)

  bool     initialized;
};

// Single global instance (defined in meshtastic_service.cpp).
extern MeshtasticService meshService;

#endif // MESHTASTIC_SERVICE_H
