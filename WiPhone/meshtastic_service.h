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

/* ── HOW MUCH CHAT HISTORY SURVIVES A REBOOT ───────────────────────────────────────────────
 * The two caps above bound what RAM holds for this session; these bound what is WRITTEN.
 * They are separate numbers because the two costs are nothing alike: the ring is PSRAM and
 * practically free, while a save blocks the whole superloop — keypad, screen and WiFi stack
 * together — for as long as the write takes.
 *
 * MEASURED on this phone (`bench`, 8 KB the same shape a real save has):
 *     SD       48-57 ms
 *     SPIFFS   2599-2845 ms      <- about FIFTY times slower
 *
 * So the card gets a generous slice and a card-less phone gets a small one, rather than a
 * pause anybody can feel. At 248 bytes a message:
 *     SD      40/chat, 200 total  ->  ~48 KB   (a fraction of a second)
 *     SPIFFS  12/chat,  60 total  ->  ~15 KB   (the size the file already was)
 *
 * ⚠ These are FLOORS on what a person sees after a reboot, not on what they see while the
 * phone is running — scrollback within a session is still MESH_MAX_PER_CHAT deep. */
#define MESH_PERSIST_PER_CHAT_SD      40
#define MESH_PERSIST_TOTAL_SD        200
#define MESH_PERSIST_PER_CHAT_FLASH   12
#define MESH_PERSIST_TOTAL_FLASH      60
/* Known-node database size. ⚠ WAS 32, AND THE TABLE WAS A PLAIN MEMBER ARRAY — so it lived in
 * BSS, i.e. the INTERNAL RAM that actually panics this phone, at 84 bytes a node. It is a
 * ps_malloc'd pointer now (same treatment as `messages`), which BOTH gives back ~2.6 KB of
 * internal RAM and makes the cap a question about PSRAM (3.6 MB free) rather than about the
 * scarce pool. Nick, on a public LongFast mesh: "let's see how big we can grow the node list
 * without any big detriments. I do enjoy seeing the mesh."
 *
 * 200 x 84 B = ~17 KB of PSRAM and the same again in the SPIFFS database (partition is 3.6 MB).
 * Saves are debounced and only fire on a real change — a new node, a name, a learned key — not
 * on every packet, so a bigger table does not mean proportionally more flash writes.
 * The binding constraint at this size is not memory, it is a person scrolling the list, which
 * is what MESH_NODE_FAVOURITE below exists to answer. */
#define MESH_MAX_NODES      200      // known-node database size (PSRAM)
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

/* ---- GPS freshness: TWO numbers, because they answer two questions --------
 *
 * MESH_GPS_FRESH_MS — "is this number still worth SHOWING?" At 1 Hz NMEA a
 * healthy receiver refreshes every second, so 120 s is 120 consecutive missed
 * epochs: unambiguously "not receiving", while still long enough to ride out a
 * walk under a hemlock and long enough that a 518 ms transmit blind spot is
 * invisible. It replaces the two hardcoded 120000UL sites that used to keep
 * this rule (resolveReference and the My node "on (fix)" label) — they must
 * never be able to disagree about whether the GPS is working.
 *
 * MESH_POS_TX_FRESH_MS — "is it worth OTHER PEOPLE'S trust?" Tighter, because
 * showing a 90-second-old position to the person holding the phone is fine (it
 * is their own position and they know where they are), while spending airtime
 * to tell a hunting party where someone is, from a receiver that has missed 30
 * straight epochs, is a guess dressed as a measurement. Skipping costs nothing:
 * the next slot is minutes away and honest silence beats a confident wrong dot
 * on somebody else's map. */
#define MESH_GPS_FRESH_MS    120000UL
#define MESH_POS_TX_FRESH_MS  30000UL

/* Fix-quality limits live in mesh_pos.h, beside the movement rule and the host tests
 * that prove both. A second copy here is how two thresholds drift apart. */

/* Reference sentinel: "measure everything from THIS PHONE'S OWN GPS", chosen
 * deliberately in Places rather than fallen into. Checked before findWaypoint(),
 * so a waypoint can never shadow it. A real Meshtastic waypoint id is a random
 * uint32, so the collision chance is 2^-32 and its only consequence is that one
 * waypoint could not be picked as a reference. Also unreachable by upgrade: the
 * only writer of `refwp` is setReferenceId() from a real waypoint id or 0, so no
 * previously-shipped firmware can have stored this value. */
#define MESH_REF_GPS         0xFFFFFFFFu

typedef enum {
  MESH_RADIO_UNINITIALIZED = 0,
  MESH_RADIO_STUBBED,               // service running, no real PHY yet
  MESH_RADIO_READY,                 // real radio initialized (future)
  MESH_RADIO_ERROR,
} MeshRadioState;

/* MeshMessage.flags bits (kept in one byte so the struct size is stable).
 *
 * ⚠ THE STRUCT SIZE IS LOAD-BEARING, WHICH IS WHY THE RECEIPT LIVES IN SPARE BITS HERE
 * RATHER THAN IN A NEW FIELD. saveDb() writes sizeof(MeshMessage) into the header and
 * loadDb() REJECTS THE WHOLE FILE if it does not match — nodes, messages AND waypoints
 * together. Growing this struct to hold a packet id would have silently binned Nick's
 * message history, his node list, and "Camp" (which the waypoint code explicitly says must
 * survive a reboot) on the first boot after the update. Spare bits cost nothing and the
 * final delivery state persists exactly like MESH_MSG_READ does. */
#define MESH_MSG_OUTGOING  0x01     // sent from this device
#define MESH_MSG_READ      0x02     // the user has viewed this message's thread
/* Delivery receipt, for OUTGOING messages only. Neither bit set = sent, no acknowledgement
 * (yet). These are DELIVERY receipts, NOT read receipts: Meshtastic has no concept of a
 * human having read anything, and the strongest signal the protocol offers is that a radio
 * acknowledged the packet. The UI wording says so rather than implying more. */
#define MESH_MSG_DELIVERED 0x04     // a routing ACK came back for this message
#define MESH_MSG_FAILED    0x08     // a routing NAK came back (no route, no key, ...)

/* How many sent-but-unacknowledged messages can be tracked at once. Acks come back in
 * seconds or never; eight is far more than a person types in that window, and the table is
 * round-robin so a flood costs the oldest receipt rather than the newest. */
#define MESH_ACK_PENDING   8

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
/* ⭐ Starred by the user. Two effects, and the second is the one that matters:
 *   1. sorts to the TOP of the node list, ahead of everything else;
 *   2. is NEVER evicted while any unstarred node remains — the same protection a learned
 *      public key earns, for the same reason. A mesh of strangers should not be able to push
 *      out the handful of nodes you actually care about. See upsertNode(). */
#define MESH_NODE_FAVOURITE     0x04

/* How many nodes can be starred. Kept small and in INTERNAL ram on purpose (128 B): this is a
 * hand-curated list, not a mesh-sized one, and it has to be consultable the instant a node is
 * created. */
#define MESH_MAX_FAVOURITES     32
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
   * posHeardMs==0 means never heard. Appended after the v2 fields; DB migrates
   * v1/v2 in place.
   * 🛑 FOR OUR OWN ENTRY THIS IS THE MANUAL PIN AND NOTHING ELSE. The GPS does
   * NOT mirror its fix in here, however tempting that looks: the only writers
   * are setMyPin()/clearMyPin(), and writing it per fix would set dbDirty on
   * every NMEA epoch and drag the ~1.5 s database save into a 1 Hz loop. The
   * self row in Nodes reads getGpsFix() live instead. */
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

  // Nodes come back in DISPLAY order: starred first, then most-recently-heard.
  int                getNodeCount() const;
  const MeshNode*    getNode(int index) const;      // NULL if out of range
  const MeshNode*    findNode(uint32_t nodeNum) const;
  bool               toggleFavourite(uint32_t nodeNum);   // returns the NEW starred state
  bool               isFavourite(uint32_t nodeNum) const;
  /* Re-sort the node list NOW. The UI calls this when it (re)builds the Nodes screen and at
   * no other time — see the note on getNode(). */
  void               refreshNodeOrder();
  /* ⚠ HOLD THE DATABASE SAVE WHILE SOMEONE IS USING THE PHONE. saveDb() blocks the superloop
   * for ~1.5 s — measured, not estimated — and everything here is one task, so that freezes
   * the keypad, the screen AND the WiFi stack together. Landing that mid-scroll is exactly
   * Nick's "freeze for a second or two and WiFi drops". The main loop calls this because it
   * is the only place that can see both the keypad and this service. */
  void               setUiIdle(bool idle) { uiIdle = idle; }
  /* The database lives on the SD card when there is one (about fifty times faster than SPIFFS
   * — measured; see meshFs()). Reported by the main loop rather than read from GUI here, so
   * this service keeps no dependency on the UI.
   *
   * 🛑 CALL THIS BEFORE setup(), NOT ONLY FROM loop(). It is the only writer of the flag
   * meshFs() consults, and setup() is where loadDb()/loadFavourites() run. Until 0.9.19 it
   * was called from loop() alone, so at load time the answer was always "no card": the phone
   * READ SPIFFS every boot while every save WROTE the card, and the whole message history
   * went into a file nothing ever opened. Defined in the .cpp because the flag it sets is a
   * file-static there. */
  void               setCardPresent(bool present);
  bool               dbOnCard() const { return cardIn; }
  /* Which file loadDb() actually read at boot: "SD", "SPIFFS", or "none". A static literal,
   * safe to hold. Exists because the bug above was invisible — the normal load path logged
   * nothing at all, so a phone reading the wrong filesystem looked exactly like a phone with
   * no history. */
  const char*        dbLoadedFrom() const { return dbSource; }
  /* How many messages the NEXT save will write, under the caps above. */
  int                persistedMessageCount() const;

  // ---- Channels ------------------------------------------------------------
  int                getChannelCount() const;
  const MeshChannel* getChannel(int index) const;
  const MeshChannel* findChannelByHash(uint8_t hash) const;
  /* 🔑 "Anything sent here is readable by any Meshtastic radio in range."
   * A PROPERTY OF THE KEY, never of the name or the index. initDefaultChannel()
   * seeds slot 0 as LongFast, but loadChannels() overwrites slot 0 from flash
   * and addChannel() merges by NAME, so a private channel can land at index 0
   * and a public one called 'hunt-group' can land anywhere. keyLen == 0 counts
   * as public because an unencrypted channel is more open than LongFast, not
   * less; a NULL channel counts as public because unknown must fail safe. */
  bool channelIsPublic(const MeshChannel* ch) const;
  /* 'booksync'/'smsmirror' carry traffic between these two devices and have no
   * human audience. Public so the position picker can LABEL them — it lists
   * them rather than hiding them, because a screen whose job is to say
   * truthfully where your location goes must not have gaps in it. */
  bool channelIsMachine(const MeshChannel* ch) const;
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

  /* The pin is a USER-DECLARED position ("I'm at camp") — a statement, not a
   * measurement, and it stays that even now the plate has a GPS. Setting it
   * persists to NVS and broadcasts one Position so COVEY's map shows this
   * phone. pinnedAtUnix is 0 when the clock was unknown at pin time. setMyPin
   * returns the ANNOUNCE result: false means the pin stuck locally but never
   * went on air — the UI must not imply otherwise.
   * 🛑 NOTHING ON THE GPS PATH WRITES THE PIN. setMyPin() does an NVS write and
   * an unconditional announce, so "let the GPS update the pin" would mean a
   * flash write and a transmission on every fix. The beacon reads gpsLatI/
   * gpsLonI directly and the pin is left exactly where the user put it. */
  bool getMyPin(int32_t* latI, int32_t* lonI, uint32_t* pinnedAtUnix) const;
  bool setMyPin(int32_t latI, int32_t lonI);              // persist + announce
  void clearMyPin();
  bool pinAnnounceOk() const { return lastAnnounceOk; }   // did the last announce transmit?

  /* Latched "positions or places changed" signal for the UI — read-and-clear.
   * Separate from loop()'s new-MESSAGE return on purpose: a waypoint arriving
   * must refresh an open Places screen but must NOT buzz, pop up, or light the
   * unread icon. (The book-sync 'receiver shows nothing' lesson, D-089.) */
  bool takePlacesNews() { bool n = placesNews; placesNews = false; return n; }

  /* Which place distances are measured FROM. Three modes, persisted as `refwp`:
   *   MESH_REF_GPS  — explicitly this phone's GPS. NEVER falls through to
   *                   anything else: a stale fix keeps its coordinates and
   *                   renames itself "last GPS", and no fix at all returns
   *                   false so the UI shows no distances. A reference frame
   *                   that moves on its own is the failure this mode exists
   *                   to rule out.
   *   a waypoint id — that place, always.
   *   0 (automatic, the shipping default) — a fresh GPS fix, else the pin.
   * resolveReference() gives the coords + a display name, false if none exist. */
  uint32_t getReferenceId() const { return refWaypointId; }
  void     setReferenceId(uint32_t id);
  bool     resolveReference(int32_t* latI, int32_t* lonI,
                            char* name, size_t nameCap) const;
  /* True when the reference is the GPS and its fix has gone stale — the Status
   * screen wants the precise age, which resolveReference() only hints at by
   * calling itself "last GPS". A separate accessor rather than a wider
   * signature, because resolveReference() has seven call sites. */
  bool     referenceIsStaleGps(uint32_t* ageMs) const;

  /* ---- The woods backplate's GPS (nmea.h) ----------------------------------
   * A LIVE fix (fresh within MESH_GPS_FRESH_MS) slots into resolveReference
   * between the chosen waypoint and the manual pin: "where am I NOW" beats a
   * pin set hours ago, but an explicit waypoint choice still beats both.
   *
   * ⚠ THIS BLOCK USED TO PROMISE "It NEVER touches announce — putting this
   * phone's position on the air stays a deliberate act (the COVEY public-
   * LongFast lesson)." That invariant is deliberately relaxed: Nick asked for
   * the GPS to be able to report to the mesh. The deliberate act did not go
   * away, it MOVED — to arming the feature (setPosInterval) and naming the
   * channel (setPosChannelName), both default-off, both persisted, with the
   * public channel behind a two-press confirm on screen. Nothing here can put
   * a byte on the air that a person did not switch on and aim.
   *
   * setGpsEnabled() mirrors WiPhone.ino's gGpsNmea in here. Without it this
   * service kept answering "GPS" with coordinates from a receiver that had
   * been switched off up to two minutes earlier. It writes nothing and blocks
   * on nothing. */
  void gpsUpdate(bool valid, int32_t latI, int32_t lonI, int sats, int hdopX10);
  bool getGpsFix(int32_t* latI, int32_t* lonI, uint32_t* ageMs,
                 int* sats, int* hdopX10) const;
  bool gpsEverFixed() const { return gpsFixMs != 0; }
  void setGpsEnabled(bool on) { gpsEnabled = on; }
  bool isGpsEnabled() const { return gpsEnabled; }

  /* ---- Periodic GPS position beacon ---------------------------------------
   * OFF by default, with NO channel chosen by default. Three separate things
   * must be true before one byte goes out: the GPS receiver is on, an interval
   * is chosen, and a channel is named. See loop()'s beacon block.
   *
   * setPosInterval() re-arms (posNextTxMs = 0) exactly as setNeighborInterval
   * does, so the keypress that switches reporting on NEVER transmits — the
   * first beacon is a full period away. For a neighbour map that is politeness;
   * for a live location it is a privacy property. */
  uint32_t    getPosInterval() const { return posIntervalSecs; }   // 0 = off
  void        setPosInterval(uint32_t secs);                       // persists (NVS)
  const char* getPosChannelName() const { return posChanName; }    // "" = unset
  void        setPosChannelName(const char* name);                 // persists (NVS)
  /* The chosen channel, re-resolved BY NAME every time. NULL when unset or no
   * longer on this phone. Names travel and indexes do not (see the ReplayHeard
   * note); a stale index would aim a live location beacon at the wrong channel
   * the first time applyChannelUrl() rewrites the table. */
  const MeshChannel* getPosChannel() const;
  /* Was the chosen channel public AT THE MOMENT IT WAS CHOSEN? The one hard
   * refusal in this feature compares that against what the channel is NOW: a
   * channel that has since been re-keyed to the stock default would otherwise
   * start broadcasting a person's location in the clear, months after they
   * armed it, in complete silence. A user who deliberately confirmed LongFast
   * has this true and is not overridden. */
  bool        posChannelWasPublic() const { return posChanWasPublic; }
  uint32_t    getPosLastTxMs() const { return posLastTxMs; }       // 0 = never
  bool        posLastSendOk()  const { return posLastOk; }
  /* True when a slot has come round and is still waiting for a fix worth sending. */
  bool        posBeaconDue()   const { return posDue; }
  /* Why nothing is being sent right now — NULL when reporting is actually
   * running (or is off, which the caller can see from getPosInterval()). The
   * string is a static literal, safe to hold. */
  const char* posBlockedReason() const;
  int         getPosSkipRuns() const { return posSkipRuns; }
  /* Bench only (serial `pos now`): build and transmit one beacon immediately,
   * ignoring the interval and the movement gate but NOT the safety rules.
   * ⚠ BLOCKS for ~518 ms in meshPhy.send(), like every other transmit here. */
  bool        sendGpsPositionNow();

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
  void      saveDb();               // persist nodes + messages (SD when there is a card)
  void      loadDb();               // restore nodes + messages (SD when there is a card)
  /* Fill `keep` (one byte per message, oldest first) with the retention decision, and return
   * how many are marked. `keep` may be NULL to just count. See mesh_retain.h. */
  int       selectPersisted(uint8_t* keep) const;
  const char* dbSource;             // which file loadDb() read; see dbLoadedFrom()
  uint32_t  chatKeyOf(uint32_t from, uint32_t to) const;   // conversation id (0 = broadcast)
  void      removeMessageAt(int idx);

  /* ---- Delivery receipts ---------------------------------------------------
   * A routing ACK/NAK identifies the message it answers by PACKET ID, but a packet id has
   * nowhere to live on MeshMessage (see the flags note above — growing the struct discards
   * the whole database). So the association is held here, in RAM, only for the seconds
   * between sending and the ack coming back.
   *
   * ⚠ IT IS KEYED ON timeMs, NOT ON THE MESSAGE'S INDEX. Indices are not stable: the store
   * compacts on removeMessageAt(), so the per-chat and global caps can shift every message
   * down while an ack is still in flight, and the receipt would land on somebody else's
   * message. timeMs is millis() at store time and two texts cannot be typed in the same
   * millisecond.
   *
   * Pending entries do NOT survive a reboot, and that is the honest behaviour: an ack for a
   * message sent before a restart can no longer be attributed to it. The RESOLVED state does
   * survive, because it lives in MeshMessage.flags, which is persisted. */
  struct PendingAck {
    uint32_t packetId;              // 0 = free slot
    uint32_t msgTimeMs;             // identifies the MeshMessage this answers
  };
  PendingAck pendingAcks[MESH_ACK_PENDING];
  uint8_t    pendingAckNext;        // round-robin; oldest unanswered is overwritten first
  uint32_t   lastStoredTimeMs;      // timeMs of the message storeMessage() just appended

  void      notePendingAck(uint32_t packetId, uint32_t msgTimeMs);   // after a successful TX
  void      resolveAck(uint32_t packetId, uint8_t errorReason);   // on an inbound ROUTING
  void      setMessageReceipt(uint32_t msgTimeMs, bool delivered, uint8_t err = 0);

  // Channels
  void      initDefaultChannel();   // channel 0 = LongFast
  bool      addChannel(const char* name, const uint8_t* key, uint8_t keyLen);   // merge
  void      loadChannels();         // from SPIFFS
  void      saveChannels();         // to SPIFFS

  /* ⭐ STARRED IDS LIVE IN THEIR OWN TINY FILE, NOT ONLY AS A BIT IN THE BIG DATABASE.
   * The node table is machine data — every entry in it is rediscoverable by listening to the
   * mesh. The stars are the one thing in there that is NOT: they are Nick's judgement about
   * which nodes matter, and nothing can regenerate them. So they get their own durability,
   * the same way channels do, instead of riding along in a ~250 KB file that is rewritten
   * whenever any node is heard.
   *
   * It also means a star SURVIVES EVICTION. The list is the source of truth and the flag on
   * MeshNode is a cache of it, re-applied whenever a node is created — so a starred node that
   * got pushed out of a full table comes back starred (and immediately eviction-proof again)
   * the moment it is heard from.
   *
   * SPIFFS, deliberately, NOT the SD card: this is 4 bytes per star, so size is irrelevant,
   * and SPIFFS is always mounted whereas a card can be absent. */
  uint32_t  favIds[MESH_MAX_FAVOURITES];
  uint8_t   favCount;
  bool      favDirty;               // set by toggleFavourite, drained by loop()
  bool      uiIdle;                 // main loop's verdict: is nobody touching the phone?
  bool      cardIn;                 // main loop's verdict: is an SD card seated? (see meshFs)

  /* ── THE DATABASE SAVE IS SPREAD ACROSS LOOP PASSES ─────────────────────────────────────
   * MEASURED 2026-08-24: SPIFFS on this part writes at about 6 KB/s, so saving the database
   * in one go blocked the superloop for ~1.5 s — and one task means the keypad, the screen and
   * the WiFi stack all stopped with it. That is Nick's "menus freeze and WiFi drops".
   * The cost is per-BYTE, not per-call (batching ~55 writes into 2 changed almost nothing), so
   * the total is irreducible — but it does NOT have to be paid all at once.
   * The image is built into PSRAM in one pass (memcpy only, no I/O), then dribbled to flash a
   * bounded chunk per loop pass. Snapshotting first also makes the write atomic in a second
   * sense: nodes and messages cannot change halfway through and tear the file.
   * ⚠ A save in flight holds an open File across passes. That is fine — it writes to the TEMP
   * file, and the rename only happens on completion, so an interrupted save leaves the old
   * database untouched exactly as before. */
  uint8_t*  saveBuf;                // PSRAM snapshot of the database image
  uint32_t  saveLen, saveOff;       // image size, and how much has reached flash
  bool      saveActive;             // (the open File itself is a static in the .cpp, so this
                                    //  header need not drag FS.h into every consumer)
  void      saveDbStep();           // one bounded chunk; called from loop()
  void      loadFavourites();       // from SPIFFS, into favIds
  void      saveFavourites();       // to SPIFFS, atomically
  void      applyFavouriteFlag(MeshNode* n) const;   // stamp the cached bit from favIds
  MeshChannel channels[MESH_MAX_CHANNELS];
  int         channelCount;

  // Messages, oldest at [0] and newest at [msgCount-1]. Allocated in PSRAM
  // (capacity MESH_MSG_CAP); capped at MESH_MAX_PER_CHAT per conversation.
  MeshMessage* messages;
  int          msgCount;

  MeshNode*   nodes;                // PSRAM (see MESH_MAX_NODES); internal fallback
  /* Display order, rebuilt lazily: starred first, then most-recently-heard. An INDEX rather
   * than sorting `nodes` itself, because findNode()/upsertNode() hand out MeshNode* that
   * callers hold across statements — re-sorting the array under them would dangle. */
  uint16_t*   nodeOrder;
  bool        orderDirty;
  int         orderCount;           // nodeCount the current order was built for
  void        rebuildNodeOrder();
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
    /* This DM is echoed into the store at QUEUE time but does not reach the air until
     * loop() drains it a tick later, by which point lastStoredTimeMs has moved on. Carry
     * the identity across so the receipt still lands on the right message. */
    uint32_t msgTimeMs;
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
  /* Mirror of WiPhone.ino's gGpsNmea, set by setGpsEnabled() from the two
   * places that toggle it (the My node row and serial `gps on|off`). Read here
   * rather than extern'd because gGpsNmea is not visible in this translation
   * unit, and because reaching into the .ino's globals from a service is how
   * the next such flag ends up out of sync too. Seeded in setup() from the same
   * NVS key the .ino reads, so there is no boot-order dependency between them. */
  bool         gpsEnabled;

  /* ---- Periodic position beacon state -------------------------------------
   * ~48 bytes of BSS, no heap, nothing allocated per broadcast. */
  uint32_t     posIntervalSecs;           // 0 = off (NVS "posint")
  char         posChanName[MESH_NAME_LEN];// target channel NAME, "" = unset (NVS "poschan")
  bool         posChanWasPublic;          // was it public when picked? (NVS "pospub")
  uint32_t     posNextTxMs;               // 0 = not armed yet (wait a full period)
  int32_t      posLastLatI, posLastLonI;  // last position actually TRANSMITTED
  bool         posLastValid;              // ...and whether there is one
  int          posSkipRuns;               // consecutive slots the movement gate ate
  uint32_t     posLastTxMs;               // millis() of the last beacon (0 = never)
  bool         posLastOk;                 // did it reach the air?
  /* ⚠ THE SLOT IS OWED, NOT SPENT. Set when the interval comes round; cleared only when the
   * beacon is actually resolved (sent, or skipped because the phone has not moved). A slot
   * that finds no usable fix stays owed and fires the moment one arrives — see the long note
   * at the tick. Never more than one is owed, so a long blind spell cannot burst. */
  bool         posDue;
  void loadPosSettings();                 // NVS, once, in setup()
  /* The one place a Position payload is built and handed to the radio. Both the
   * manual pin announce and the GPS beacon go through it, so there is exactly
   * one set of bytes and one log line to reason about. */
  bool sendPositionOn(int32_t latI, int32_t lonI, const MeshChannel* ch, const char* why);
  bool sendGpsPosition();                  // the scheduled beacon (safety rules inside)

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
