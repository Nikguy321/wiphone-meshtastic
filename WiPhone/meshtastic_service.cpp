/*
 * meshtastic_service.cpp — WiPhone Meshtastic integration (Phase 3 shell)
 *
 * Stubbed radio: no hardware access yet. The public API is complete so the UI
 * can be built and exercised end-to-end. The real SX1276/Meshtastic PHY will
 * replace the stub internals (storeMessage on RX, actual TX in send*) later.
 */

#include "meshtastic_service.h"

extern uint32_t heapDelta(const char* what, uint32_t before);  // WiPhone.ino - ratchet instrument
#include "esp_heap_caps.h"
#include "config.h"            // MESHTASTIC_PHY toggle
#include "mesh_pki.h"          // PKC keypair/derive (portable — used in stub builds too)
#include "mesh_pos.h"
#include "replay_proto.h"   // mesh history replay (docs/replay-spec.md)
#include "neighbor_info.h"  // NeighborInfo (portnum 71) encoding

/* Replay sizing. The ring and the reply slab live in PSRAM: 64×~184 B + 24×256 B
 * ≈ 18 KB of the 3.6 MB pool — deliberately NOTHING from internal heap, which
 * this phone measures in single-digit KB on a bad day. */
#define REPLAY_RING_CAP   64
#define REPLAY_MAX_PKTS   24
#define REPLAY_PKT_STRIDE 256
#define REPLAY_TX_GAP_MS  3000          // Position/Waypoint payloads + distance math (portable)
#include <Preferences.h>      // NVS-backed node name persistence
#include "booksync_inbox.h"   // book-sync packets are diverted out of the chat list
#include "sms_mirror_rx.h"    // ...and so are texts mirrored from COVEY

#include "clock.h"            // ntpClock, for when a parked packet arrived
#include <SPIFFS.h>           // node + message history persistence
#include <SD.h>               // ...which now lives HERE when a card is in; see meshFs()
#include "mesh_retain.h"      // which messages are worth writing down

/* Mirrors MeshtasticService::cardIn so the file-scope filesystem helpers can see it without
 * every one of them taking a `this`. Refreshed at the top of the service's loop(). */
static bool s_meshCardIn = false;

// Debounce persistence writes so a burst of traffic doesn't hammer flash.
#define MESH_SAVE_DEBOUNCE_MS  10000

// Hop limit used for originated packets (mirrors MeshtasticService::myHopLimit
// so the static TX helpers can read it). Updated in setup()/setHopLimit().
static uint8_t s_hopLimit = 3;

#ifdef MESHTASTIC_PHY
#include "mesh_phy.h"
#include "mesh_packet.h"
#include "mesh_crypto.h"
#include <esp_system.h>          // esp_random()

// Meshtastic PortNum values we care about (from the Data protobuf).
#define MESH_PORT_TEXT_MESSAGE  1
#define MESH_PORT_POSITION      3   // COVEY broadcasts one every 5 minutes
#define MESH_PORT_NODEINFO      4
#define MESH_PORT_ROUTING       5   // ACK/NAK carrier: Routing{error_reason}
#define MESH_PORT_WAYPOINT      8   // camp / truck / stand pins from COVEY's map

// Default hop limit for packets we originate (Meshtastic default is 3).
#define MESH_TX_HOP_LIMIT       3

// Minimal parser for a decrypted Meshtastic Data protobuf. Returns field 1
// (portnum, varint) and, via the out params, field 2 (payload bytes). The
// payload meaning depends on portnum: UTF-8 text for TEXT_MESSAGE, a nested
// User protobuf for NODEINFO, etc. Bounds-checked against `dlen`.
/* ⚠ `wantRespOut` is optional and may be NULL. It carries Data field 3, want_response —
 * how every other Meshtastic node asks "tell me who you are". This was previously only ever
 * SET on the send side and never read on receive, so the phone asked the question and never
 * answered it: it appeared on other radios as a bare node number with no name until it
 * happened to announce on its own. See the reply in the NODEINFO branch of the receive path. */
/* `requestIdOut` (optional) carries Data field 6, request_id — on a ROUTING packet it
 * names the packet an ACK/NAK is answering, which is the phone's only proof of DM
 * delivery. */
static int meshParseData(const uint8_t* data, size_t dlen,
                         const uint8_t** payloadOut, size_t* payloadLenOut,
                         bool* wantRespOut = NULL, uint32_t* requestIdOut = NULL) {
  int portnum = -1;
  const uint8_t* payload = NULL;
  size_t payloadLen = 0;
  if (wantRespOut) {
    *wantRespOut = false;
  }
  if (requestIdOut) {
    *requestIdOut = 0;
  }
  size_t i = 0;
  while (i < dlen) {
    uint8_t tag = data[i++];
    uint8_t field = tag >> 3;
    uint8_t wire = tag & 0x07;
    if (wire == 0) {                       // varint
      uint32_t v = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { v |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { v |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 1) {
        portnum = (int)v;
      } else if (field == 3 && wantRespOut) {
        *wantRespOut = (v != 0);
      }
    } else if (wire == 2) {                // length-delimited
      uint32_t l = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { l |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { l |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 2) { payload = data + i; payloadLen = l; }
      i += l;
    } else if (wire == 5) {                // fixed32
      if (field == 6 && requestIdOut && i + 4 <= dlen) {
        *requestIdOut = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                        ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
      }
      i += 4;
    }
    else if (wire == 1) { i += 8; }
    else break;
  }
  if (payload) {                           // clamp to the actual buffer
    size_t avail = (size_t)(data + dlen - payload);
    if (payloadLen > avail) payloadLen = avail;
  }
  if (payloadOut)    *payloadOut = payload;
  if (payloadLenOut) *payloadLenOut = payloadLen;
  return portnum;
}

// Extract a display name from a Meshtastic User protobuf (the NODEINFO payload):
// prefer long_name (field 2), fall back to short_name (field 3). Returns true if
// a non-empty name was copied into `out` (null-terminated).
// `pubKeyOut`/`hasKeyOut` (optional) receive field 8, public_key — the node's
// X25519 key, ONLY when it is exactly 32 bytes (stock strips it for ham/licensed
// operators, so absence is normal).
static bool meshParseUserName(const uint8_t* data, size_t dlen, char* out, size_t outCap,
                              uint8_t* pubKeyOut = NULL, bool* hasKeyOut = NULL) {
  const uint8_t* longName = NULL;  size_t longLen = 0;
  const uint8_t* shortName = NULL; size_t shortLen = 0;
  if (hasKeyOut) {
    *hasKeyOut = false;
  }
  size_t i = 0;
  while (i < dlen) {
    uint8_t tag = data[i++];
    uint8_t field = tag >> 3;
    uint8_t wire = tag & 0x07;
    if (wire == 2) {
      uint32_t l = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { l |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { l |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 2)      { longName = data + i;  longLen = l; }
      else if (field == 3) { shortName = data + i; shortLen = l; }
      else if (field == 8 && l == MESH_PKI_KEY_LEN && pubKeyOut && hasKeyOut &&
               i + MESH_PKI_KEY_LEN <= dlen) {
        memcpy(pubKeyOut, data + i, MESH_PKI_KEY_LEN);
        *hasKeyOut = true;
      }
      i += l;
    } else if (wire == 0) {
      while (i < dlen && (data[i] & 0x80)) i++;
      if (i < dlen) i++;
    } else if (wire == 5) { i += 4; }
    else if (wire == 1) { i += 8; }
    else break;
  }
  const uint8_t* src = longName ? longName : shortName;
  size_t srcLen      = longName ? longLen  : shortLen;
  if (!src || srcLen == 0) {
    return false;
  }
  size_t avail = (size_t)(data + dlen - src);
  size_t n = srcLen < avail ? srcLen : avail;
  if (n > outCap - 1) n = outCap - 1;
  memcpy(out, src, n);
  out[n] = '\0';
  return true;
}

// Append a protobuf varint length-delimited field (tag + length + bytes).
static size_t pbBytes(uint8_t* out, int field, const uint8_t* val, size_t len) {
  size_t d = 0;
  out[d++] = (uint8_t)((field << 3) | 2);
  if (len < 128) {
    out[d++] = (uint8_t)len;
  } else {
    out[d++] = (uint8_t)((len & 0x7f) | 0x80);
    out[d++] = (uint8_t)(len >> 7);
  }
  memcpy(out + d, val, len);
  return d + len;
}

static size_t pbString(uint8_t* out, int field, const char* s) {
  return pbBytes(out, field, (const uint8_t*)s, strlen(s));
}

// Serialize a Data protobuf: { portnum, payload, [want_response], [request_id],
// bitfield }. The bitfield (field 9, added in Meshtastic 2.5) mirrors stock's
// Router: bit0 = ok-to-MQTT (we say no — private mesh), bit1 = want_response.
// request_id (field 6, fixed32) is nonzero only on ACK/NAK Routing replies.
static size_t meshBuildData(uint8_t* data, int portnum,
                            const uint8_t* payload, size_t payloadLen,
                            bool wantResponse, uint32_t requestId) {
  size_t d = 0;
  data[d++] = 0x08;                          // field 1 portnum (varint)
  data[d++] = (uint8_t)portnum;
  d += pbBytes(data + d, 2, payload, payloadLen);   // field 2 payload
  if (wantResponse) {
    data[d++] = 0x18;                        // field 3 want_response (varint)
    data[d++] = 0x01;
  }
  if (requestId != 0) {
    data[d++] = 0x35;                        // field 6 request_id (fixed32)
    data[d++] = (uint8_t)(requestId >> 0);
    data[d++] = (uint8_t)(requestId >> 8);
    data[d++] = (uint8_t)(requestId >> 16);
    data[d++] = (uint8_t)(requestId >> 24);
  }
  data[d++] = 0x48;                          // field 9 bitfield (varint)
  data[d++] = wantResponse ? 0x02 : 0x00;
  return d;
}

static void meshFillHeader(MeshPacketHeader* hdr, uint32_t sender, uint32_t dest,
                           uint32_t packetId, uint8_t channelHash) {
  hdr->dest        = dest;
  hdr->sender      = sender;
  hdr->packetId    = packetId;
  hdr->flags       = (s_hopLimit & MESH_FLAGS_HOP_LIMIT_MASK) |
                     (s_hopLimit << MESH_FLAGS_HOP_START_SHIFT);
  hdr->channelHash = channelHash;
  hdr->nextHop     = 0;
  hdr->relayNode   = (uint8_t)(sender & 0xFF);   // originator is the first relay

}

/* The id of the packet the last meshTx* call put on the air. A routing ACK names the message
 * it answers by packet id, and the send functions generate that id internally — this hands it
 * back to the caller without changing four signatures and every call site. Written on the
 * radio path and read immediately after, both in loop()/GUI context on the same core. */
static uint32_t s_lastTxPacketId = 0;

/* millis() when the radio last finished a transmission. meshPhy.send() BLOCKS
 * for the whole airtime — 518 ms for a 37-byte position frame at the LongFast
 * registers this repo writes (SF11/BW250/CR4-5, computed in mesh_phy.cpp's
 * configureLongFast) — and the superloop makes no progress during it. Two of
 * those back to back is ~1.05 s of frozen keypad and GUI; the watchdog is 20 s
 * so it is not a reset, but it is visibly janky and three would be worse.
 * The position beacon consults this and simply waits a pass. Stamped at both
 * transmit sites; 0 at boot means the first beacon is never gated. */
static uint32_t s_lastPhyTxMs = 0;

static uint32_t meshNewPacketId() {
  uint32_t packetId = esp_random();
  return packetId == 0 ? 1 : packetId;
}

// Build a Data protobuf and transmit it as a CHANNEL-encrypted (AES-CTR)
// Meshtastic packet — broadcasts, and the pre-PKC "legacy" DM form.
static bool meshTxData(uint32_t sender, uint32_t dest, int portnum,
                       const uint8_t* payload, size_t payloadLen, bool wantResponse,
                       const uint8_t* key, uint8_t keyLen, uint8_t channelHash,
                       uint32_t requestId = 0, bool wantAck = false) {
  uint8_t data[24 + MESH_PHY_MAX_PAYLOAD];
  size_t d = meshBuildData(data, portnum, payload, payloadLen, wantResponse, requestId);

  size_t pktLen = MESH_HEADER_LEN + d;
  if (pktLen > 255) {
    /* The PHY takes a uint8_t length: 256 would WRAP TO ZERO and "send"
     * nothing. Reachable with a maximum-length text — refuse honestly. */
    log_e("MESH TX REFUSED: %uB exceeds the 255B LoRa frame", (unsigned)pktLen);
    return false;
  }

  uint32_t packetId = meshNewPacketId();
  MeshPacketHeader hdr;
  meshFillHeader(&hdr, sender, dest, packetId, channelHash);
  /* ⚠ DEFAULTED OFF, AND IT MUST STAY THAT WAY. This function carries NodeInfo, position,
   * neighbour info AND our own outgoing routing ACKs. Setting want_ack for everything would
   * ask the mesh to acknowledge every beacon, and asking for an ack ON an ack is a loop.
   * Only the text path opts in. */
  if (wantAck) {
    hdr.flags |= MESH_FLAGS_WANT_ACK;
  }
  s_lastTxPacketId = packetId;

  uint8_t pkt[MESH_HEADER_LEN + 24 + MESH_PHY_MAX_PAYLOAD];
  memcpy(pkt, &hdr, MESH_HEADER_LEN);
  if (!meshCryptCtr(key, keyLen, sender, packetId, data, d, pkt + MESH_HEADER_LEN)) {
    return false;
  }
  log_i("Mesh TX port=%d to 0x%08X ch=0x%02X id=0x%08X (%uB)",
        portnum, dest, channelHash, packetId, (unsigned)pktLen);
  const bool sent = meshPhy.send(pkt, (uint8_t)pktLen);
  s_lastPhyTxMs = millis();   // stamped even on failure: the radio was still busy
  return sent;
}

// Build a Data protobuf and transmit it PKI-encrypted (AES-256-CCM under the
// X25519-derived key) — how 2.5+ nodes require every DM. On the air the
// channel-hash byte is 0x00, the frame grows by MESH_PKI_OVERHEAD (12B), and
// the receiver finds us by trying its known keys against the CCM tag.
static bool meshTxDataPki(uint32_t sender, uint32_t dest, int portnum,
                          const uint8_t* payload, size_t payloadLen,
                          const uint8_t sessionKey[MESH_PKI_KEY_LEN],
                          uint32_t requestId = 0, bool wantAck = false) {
  uint8_t data[24 + MESH_PHY_MAX_PAYLOAD];
  size_t d = meshBuildData(data, portnum, payload, payloadLen, false, requestId);

  size_t pktLen = MESH_HEADER_LEN + d + MESH_PKI_OVERHEAD;
  if (pktLen > 255) {
    log_e("MESH PKI TX REFUSED: %uB exceeds the 255B LoRa frame", (unsigned)pktLen);
    return false;
  }

  uint32_t packetId = meshNewPacketId();
  MeshPacketHeader hdr;
  meshFillHeader(&hdr, sender, dest, packetId, 0x00);    // hash 0 = PKI on the air
  s_lastTxPacketId = packetId;
  /* want_ack asks the peer for a Routing ACK. No retransmit machinery here, but
   * the ACK line in the serial log (`MESH DM ACK ... err=0`) is per-message
   * proof of delivery, and it is how the 2026-08-19 interop was verified. */
  if (wantAck) {
    hdr.flags |= MESH_FLAGS_WANT_ACK;
  }

  uint8_t pkt[MESH_HEADER_LEN + 24 + MESH_PHY_MAX_PAYLOAD + MESH_PKI_OVERHEAD];
  memcpy(pkt, &hdr, MESH_HEADER_LEN);
  uint32_t extraNonce = esp_random();
  if (meshPkiEncrypt(sessionKey, sender, packetId, extraNonce,
                     data, d, pkt + MESH_HEADER_LEN) == 0) {
    return false;
  }
  log_i("Mesh PKI TX port=%d to 0x%08X id=0x%08X (%uB)",
        portnum, dest, packetId, (unsigned)pktLen);
  const bool sent = meshPhy.send(pkt, (uint8_t)pktLen);
  s_lastPhyTxMs = millis();   // see s_lastPhyTxMs: the beacon spaces itself off this
  return sent;
}

// ACK a DM: Data{portnum=ROUTING, payload=Routing{error_reason=NONE}, request_id}.
// CHANNEL-encrypted on the primary channel even for PKI DMs — stock excludes
// ROUTING from PKC (Router.cpp) and sends ACKs with the channel-index-0 key.
static bool meshTxAck(uint32_t sender, uint32_t dest, uint32_t requestId,
                      const MeshChannel* ch) {
  static const uint8_t ackNone[2] = { 0x18, 0x00 };   // Routing{error_reason=NONE}
  return meshTxData(sender, dest, MESH_PORT_ROUTING, ackNone, sizeof(ackNone), false,
                    ch->key, ch->keyLen, ch->hash, requestId);
}

static bool meshTxText(uint32_t sender, uint32_t dest, const char* text,
                       const MeshChannel* ch, bool wantAck = false) {
  size_t textLen = strlen(text);
  if (textLen == 0 || textLen > MESH_TEXT_LEN - 1 || !ch) {
    return false;
  }
  return meshTxData(sender, dest, MESH_PORT_TEXT_MESSAGE, (const uint8_t*)text, textLen,
                    false, ch->key, ch->keyLen, ch->hash, 0, wantAck);
}

// Broadcast our NodeInfo: a User protobuf { id, long_name, short_name,
// public_key }. wantResponse solicits other nodes to reply with their own.
// The public key (field 8) is the whole reason 2.5+ nodes will DM us at all:
// without it every DM from a stock node dies with PKI_SEND_FAIL_PUBLIC_KEY.
static bool meshTxNodeInfo(uint32_t sender, const char* longName, const char* shortName,
                           bool wantResponse, const MeshChannel* ch,
                           const uint8_t* pkiPub) {
  if (!ch) {
    return false;
  }
  char id[12];
  snprintf(id, sizeof(id), "!%08x", sender);

  uint8_t user[128];
  size_t u = 0;
  u += pbString(user + u, 1, id);            // field 1 id
  u += pbString(user + u, 2, longName);      // field 2 long_name
  u += pbString(user + u, 3, shortName);     // field 3 short_name
  if (pkiPub) {
    u += pbBytes(user + u, 8, pkiPub, MESH_PKI_KEY_LEN);   // field 8 public_key
  }

  return meshTxData(sender, MESH_ADDR_BROADCAST_ONAIR, MESH_PORT_NODEINFO,
                    user, u, wantResponse, ch->key, ch->keyLen, ch->hash);
}
#endif

// Node identity is derived from the ESP32 eFuse MAC, same source Meshtastic
// uses for node numbers. chipId is populated in the WiPhone setup() before
// meshService.setup() runs.
extern uint32_t chipId;

MeshtasticService meshService;

MeshtasticService::MeshtasticService()
  : msgCount(0), nodeCount(0),
    radioState(MESH_RADIO_UNINITIALIZED),
    region("US"), channelName("LongFast"), modemPreset("LongFast"),
    myNodeNum(0), recentPktPos(0), dbDirty(false), dbSource("none"), lastSaveMs(0), initialized(false),
    nodes(NULL), nodeOrder(NULL), orderDirty(true), orderCount(0), favCount(0), favDirty(false),
    uiIdle(true), cardIn(false), saveBuf(NULL), saveLen(0), saveOff(0), saveActive(false) {
  messages = NULL;
  msgCount = 0;
  channelCount = 0;
  myHopLimit = 3;
  /* ⚠ NO memset OF `nodes` HERE. It was `memset(nodes, 0, sizeof(nodes))` back when the
   * table was a fixed member array, where sizeof gave the whole 32*84 bytes. `nodes` is a
   * POINTER now, so sizeof(nodes) is 4 — and at construction time it is still NULL, which
   * made this memset(NULL, 0, 4): a StoreProhibited panic in a GLOBAL CONSTRUCTOR, i.e. a
   * boot loop before setup() ever ran. It compiles without a murmur. setup() allocates the
   * table and zeroes it there; there is nothing to clear at this point. */
  memset(channels, 0, sizeof(channels));
  memset(recentPktIds, 0, sizeof(recentPktIds));
  memset(rebroadcast, 0, sizeof(rebroadcast));
  myLongName[0] = '\0';
  myShortName[0] = '\0';
  shortNameCustom = false;
  nextNodeInfoMs = 0;
  lastNodeInfoTxMs = 0;
  memset(myPkiPriv, 0, sizeof(myPkiPriv));
  memset(myPkiPub, 0, sizeof(myPkiPub));
  pkiReady = false;
  pkiCacheClear();
  memset(pendingDm, 0, sizeof(pendingDm));
  memset(pendingAcks, 0, sizeof(pendingAcks));
  pendingAckNext   = 0;
  lastStoredTimeMs = 0;
  memset(recentAckIds, 0, sizeof(recentAckIds));
  recentAckPos = 0;
  memset(waypoints, 0, sizeof(waypoints));
  waypointCount = 0;
  myPinLatI = myPinLonI = 0;
  myPinSet = false;
  myPinAtUnix = 0;
  refWaypointId = 0;
  placesNews = false;
  lastAnnounceOk = false;
  nextWpSweepMs = 0;
  gpsLatI = gpsLonI = 0;
  gpsFixMs = 0;
  gpsSats = gpsHdopX10 = -1;
  gpsEnabled = false;
  /* Beacon: off, aimed nowhere, never transmitted. setup() may load a stored
   * choice over the top, but a construction that armed anything would arm it
   * before setup() has decided whether the radio even exists. */
  posIntervalSecs = 0;
  posChanName[0] = '\0';
  posChanWasPublic = false;
  posNextTxMs = 0;
  posLastLatI = posLastLonI = 0;
  posLastValid = false;
  posSkipRuns = 0;
  posLastTxMs = 0;
  posLastOk = false;
  posDue = false;
}

// ---- Positions & places -----------------------------------------------------

int MeshtasticService::getWaypointCount() const {
  return waypointCount;
}

const MeshWaypoint* MeshtasticService::getWaypoint(int index) const {
  if (index < 0 || index >= waypointCount) {
    return NULL;
  }
  return &waypoints[index];
}

const MeshWaypoint* MeshtasticService::findWaypoint(uint32_t id) const {
  for (int i = 0; i < waypointCount; i++) {
    if (waypoints[i].id == id) {
      return &waypoints[i];
    }
  }
  return NULL;
}

void MeshtasticService::upsertWaypoint(uint32_t id, int32_t latI, int32_t lonI,
                                       uint32_t expire, uint32_t lockedTo, const char* name) {
  if (id == 0) {
    return;
  }
  MeshWaypoint* slot = NULL;
  for (int i = 0; i < waypointCount; i++) {
    if (waypoints[i].id == id) {
      slot = &waypoints[i];
      break;
    }
  }
  if (!slot) {
    if (waypointCount < MESH_MAX_WAYPOINTS) {
      slot = &waypoints[waypointCount++];
    } else {
      /* Full: evict the least-recently-heard (same policy as the node table).
       * ⚠ Signed difference, like the node eviction: restored entries are
       * rebased to heardMs=0 at load, and plain '<' would invert at the
       * millis() wrap (the review caught both). */
      slot = &waypoints[0];
      for (int i = 1; i < MESH_MAX_WAYPOINTS; i++) {
        if ((int32_t)(waypoints[i].heardMs - slot->heardMs) < 0) {
          slot = &waypoints[i];
        }
      }
    }
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
  }
  slot->latI = latI;
  slot->lonI = lonI;
  slot->expire = expire;
  slot->lockedTo = lockedTo;
  slot->heardMs = millis();
  placesNews = true;
  if (name && name[0]) {
    strlcpy(slot->name, name, sizeof(slot->name));
  } else if (!slot->name[0]) {
    snprintf(slot->name, sizeof(slot->name), "wp-%u", (unsigned)(id & 0xFFFF));
  }
  dbDirty = true;
  log_e("MESH WAYPOINT: '%s' id=%u %d.%05d,%d.%05d%s", slot->name, (unsigned)id,
        (int)(latI / 10000000), abs((int)((latI % 10000000) / 100)),
        (int)(lonI / 10000000), abs((int)((lonI % 10000000) / 100)),
        expire ? " (expires)" : "");
}

bool MeshtasticService::getMyPin(int32_t* latI, int32_t* lonI, uint32_t* pinnedAtUnix) const {
  if (!myPinSet) {
    return false;
  }
  if (latI) *latI = myPinLatI;
  if (lonI) *lonI = myPinLonI;
  if (pinnedAtUnix) *pinnedAtUnix = myPinAtUnix;
  return true;
}

bool MeshtasticService::setMyPin(int32_t latI, int32_t lonI) {
  myPinLatI = latI;
  myPinLonI = lonI;
  myPinSet = true;
  myPinAtUnix = ntpClock.isTimeKnown() ? (uint32_t)ntpClock.getExactUnixTime() : 0;
  Preferences prefs;
  prefs.begin("wpmesh", false);
  prefs.putInt("pinlat", myPinLatI);
  prefs.putInt("pinlon", myPinLonI);
  prefs.putUInt("pinat", myPinAtUnix);
  prefs.end();
  // Mirror into our own node entry so every list treats us like any other node.
  MeshNode* self = upsertNode(myNodeNum, NULL);
  if (self) {
    self->latI = latI;
    self->lonI = lonI;
    self->posHeardMs = millis();
  }
  placesNews = true;
  return announceMyPosition();
}

void MeshtasticService::clearMyPin() {
  myPinSet = false;
  myPinAtUnix = 0;
  Preferences prefs;
  prefs.begin("wpmesh", false);
  prefs.remove("pinlat");
  prefs.remove("pinlon");
  prefs.remove("pinat");
  prefs.end();
  MeshNode* self = upsertNode(myNodeNum, NULL);
  if (self) {
    self->posHeardMs = 0;
  }
}

void MeshtasticService::loadPin() {
  Preferences prefs;
  prefs.begin("wpmesh", true);
  if (prefs.isKey("pinlat") && prefs.isKey("pinlon")) {
    myPinLatI = prefs.getInt("pinlat", 0);
    myPinLonI = prefs.getInt("pinlon", 0);
    myPinAtUnix = prefs.getUInt("pinat", 0);
    myPinSet = true;
  }
  refWaypointId = prefs.getUInt("refwp", 0);
  prefs.end();
}

void MeshtasticService::setReferenceId(uint32_t id) {
  refWaypointId = id;
  Preferences prefs;
  prefs.begin("wpmesh", false);
  prefs.putUInt("refwp", id);
  prefs.end();
}

/* The point distances are measured from. Explicit GPS wins outright; then the
 * chosen waypoint; then a LIVE GPS fix (automatic mode); then the manual pin.
 * False when none exist — the UI then simply shows no distances, honestly. */
bool MeshtasticService::resolveReference(int32_t* latI, int32_t* lonI,
                                         char* name, size_t nameCap) const {
  /* 🔑 EXPLICIT GPS, checked FIRST and never falling through. Somebody chose
   * "measure from where I am", so the frame must not quietly become somewhere
   * else the moment the sky closes: a reference that moves on its own turns
   * every distance on the screen into a confident wrong answer, and in the
   * woods that is worse than no answer. A stale fix therefore KEEPS its
   * coordinates and says so in the NAME — "last GPS" rides along on every
   * distance line — and a receiver that is off, or has never fixed, returns
   * false so nothing is drawn at all. */
  if (refWaypointId == MESH_REF_GPS) {
    /* 🛑 THE FIX-QUALITY GATE BELONGS HERE TOO, AND FOR THE REASON WRITTEN FOUR LINES ABOVE.
     * meshPosFixUsable() had exactly ONE production caller — the TRANSMIT path — under a
     * measured note that a `sats=3 hdop=6.4` fix was TWENTY KILOMETRES WRONG. So the phone
     * refused to put that fix on the air and then computed every distance and bearing on your
     * own screen from it. "A confident wrong answer, and in the woods that is worse than no
     * answer" is this function's own argument against what it was doing. */
    if (!gpsEnabled || gpsFixMs == 0 || !meshPosFixUsable(gpsSats, gpsHdopX10)) {
      return false;
    }
    const uint32_t age = (uint32_t)(millis() - gpsFixMs);
    if (latI) *latI = gpsLatI;
    if (lonI) *lonI = gpsLonI;
    /* A word, not a number: the distance lines already carry the position's own
     * age ("1.4km NE of last GPS, 4 min ago") and two ages in one line reads as
     * a puzzle. The precise age lives on the Status screen. */
    if (name) strlcpy(name, age < MESH_GPS_FRESH_MS ? "GPS" : "last GPS", nameCap);
    return true;
  }
  const MeshWaypoint* wp = refWaypointId ? findWaypoint(refWaypointId) : NULL;
  if (wp) {
    if (latI) *latI = wp->latI;
    if (lonI) *lonI = wp->lonI;
    if (name) strlcpy(name, wp->name, nameCap);
    return true;
  }
  /* AUTOMATIC (refWaypointId == 0), the shipping default and the mode every
   * upgraded phone is in. Unchanged apart from the gpsEnabled term: without it
   * this answered "GPS" for up to MESH_GPS_FRESH_MS after the receiver was
   * switched off, because nothing told the service the toggle had moved. */
  if (gpsEnabled && gpsFixMs != 0 && meshPosFixUsable(gpsSats, gpsHdopX10) &&
      (uint32_t)(millis() - gpsFixMs) < MESH_GPS_FRESH_MS) {   // same gate; falls through to the pin
    if (latI) *latI = gpsLatI;
    if (lonI) *lonI = gpsLonI;
    if (name) strlcpy(name, "GPS", nameCap);
    return true;
  }
  if (myPinSet) {
    if (latI) *latI = myPinLatI;
    if (lonI) *lonI = myPinLonI;
    if (name) strlcpy(name, "me", nameCap);
    return true;
  }
  return false;
}

bool MeshtasticService::referenceIsStaleGps(uint32_t* ageMs) const {
  if (refWaypointId != MESH_REF_GPS || !gpsEnabled || gpsFixMs == 0) {
    return false;
  }
  const uint32_t age = (uint32_t)(millis() - gpsFixMs);
  if (age < MESH_GPS_FRESH_MS) {
    return false;
  }
  if (ageMs) *ageMs = age;
  return true;
}

/* Fed from the loop whenever the NMEA reader completes an RMC/GGA (nmea.h).
 * Sats/HDOP flow even without a fix — "0 fix, 7 sats" is bench information.
 * The first fix ever is log_e'd once: that is the moment the woods plate's
 * GPS half is PROVEN, and it should be visible in a release build's log. */
void MeshtasticService::gpsUpdate(bool valid, int32_t latI, int32_t lonI,
                                  int sats, int hdopX10) {
  if (sats >= 0) {
    gpsSats = sats;
  }
  if (hdopX10 >= 0) {
    gpsHdopX10 = hdopX10;
  }
  if (valid) {
    bool first = (gpsFixMs == 0);
    gpsLatI = latI;
    gpsLonI = lonI;
    gpsFixMs = millis();
    if (gpsFixMs == 0) {
      gpsFixMs = 1;                     // millis()==0 must not read as "never"
    }
    if (first) {
      log_e("GPS: FIRST FIX %d.%05d,%d.%05d sats=%d hdop=%d.%d",
            (int)(latI / 10000000), abs((int)((latI % 10000000) / 100)),
            (int)(lonI / 10000000), abs((int)((lonI % 10000000) / 100)),
            gpsSats, gpsHdopX10 >= 0 ? gpsHdopX10 / 10 : -1,
            gpsHdopX10 >= 0 ? gpsHdopX10 % 10 : 0);
    }
  }
}

bool MeshtasticService::getGpsFix(int32_t* latI, int32_t* lonI, uint32_t* ageMs,
                                  int* sats, int* hdopX10) const {
  if (sats) {
    *sats = gpsSats;
  }
  if (hdopX10) {
    *hdopX10 = gpsHdopX10;
  }
  if (gpsFixMs == 0) {
    return false;
  }
  if (latI) {
    *latI = gpsLatI;
  }
  if (lonI) {
    *lonI = gpsLonI;
  }
  if (ageMs) {
    *ageMs = (uint32_t)(millis() - gpsFixMs);
  }
  return true;
}

bool MeshtasticService::channelIsPublic(const MeshChannel* ch) const {
  /* Unknown fails SAFE. A NULL channel here means "the thing you meant to aim
   * at is not on this phone", and calling that private would be the one wrong
   * answer that costs somebody their location. */
  if (!ch) {
    return true;
  }
  /* keyLen 0 = no encryption at all, which is more open than LongFast, not
   * less — folded in so a single call answers "can strangers read this". */
  if (ch->keyLen == 0) {
    return true;
  }
  return ch->keyLen == 16 && memcmp(ch->key, meshDefaultKey(), 16) == 0;
}

/* 🔑 THE ONE PLACE A POSITION BECOMES BYTES ON THE AIR. Both callers — the
 * manual pin announce and the periodic GPS beacon — come through here, so
 * there is one payload, one set of TX flags and one log line to reason about.
 * `why` is a short literal that names the PROVENANCE: the wire format cannot
 * distinguish a user's declaration from a receiver's measurement, and a log
 * that cannot either is a log that cannot answer "why did my phone just tell
 * the mesh where I am". Nothing here allocates; `pos` is 16 stack bytes. */
bool MeshtasticService::sendPositionOn(int32_t latI, int32_t lonI,
                                       const MeshChannel* ch, const char* why) {
#ifdef MESHTASTIC_PHY
  if (!ch || radioState != MESH_RADIO_READY || channelCount == 0) {
    log_e("MESH POSITION NOT SENT (%s): chan=%s radio=%d channels=%d",
          why, ch ? ch->name : "NONE", (int)radioState, channelCount);
    return false;
  }
  uint8_t pos[16];
  /* 🛑 getExactUtcTime(), NOT getExactUnixTime(). This timestamp GOES ON THE AIR — meshPosBuild
   * writes it into Position field 4, COVEY reads it verbatim and renders `time.time() - ts`.
   * getExactUnixTime() is the LOCAL-shifted epoch (clock.h:77 adds timeOffsetSeconds, and
   * WiPhone.ino:1479 loads `[time] zone` at boot; the shipped config is zone=-8), so every
   * position this phone transmitted read **EIGHT HOURS STALE on COVEY's map**.
   * ⚠ In the woods that is the difference between "he moved twenty minutes ago" and "that fix
   * is from this morning" — and it is the same shift as the 7.7-hour-old position that was
   * written up on 2026-08-25 as the phone being indoors.
   * The identical trap was found and fixed in the replay path (see the note at the
   * getExactUtcTime() call there); it was never swept to the other senders. */
  const size_t n = meshPosBuild(pos, latI, lonI,
                                ntpClock.isTimeKnown() ? (uint32_t)ntpClock.getExactUtcTime() : 0);
  /* wantResponse and wantAck both false, like every other beacon here: a
   * periodic broadcast that also demanded replies is a mesh-wide storm. */
  const bool ok = meshTxData(myNodeNum, MESH_ADDR_BROADCAST_ONAIR, MESH_PORT_POSITION,
                             pos, n, false, ch->key, ch->keyLen, ch->hash);
  log_e("MESH POSITION %s (%s) on '%s'%s: %d.%05d,%d.%05d  %uB payload",
        ok ? "SENT" : "FAILED", why, ch->name,
        channelIsPublic(ch) ? " [PUBLIC - readable by any radio in range]" : "",
        (int)(latI / 10000000), abs((int)((latI % 10000000) / 100)),
        (int)(lonI / 10000000), abs((int)((lonI % 10000000) / 100)), (unsigned)n);
  return ok;
#else
  (void)latI; (void)lonI; (void)ch; (void)why;
  return false;
#endif
}

bool MeshtasticService::announceMyPosition() {
#ifdef MESHTASTIC_PHY
  if (!myPinSet || radioState != MESH_RADIO_READY || channelCount == 0) {
    /* log_e, because "the pin looks set but never went on air" reads as
     * "they know where I am" when nobody does — the worst kind of silence. */
    log_e("MESH POSITION NOT SENT: pin=%d radio=%d channels=%d",
          (int)myPinSet, (int)radioState, channelCount);
    lastAnnounceOk = false;
    return false;
  }
  /* Prefer the first channel with a PRIVATE key. channels[0] is the stock
   * default (LongFast, the well-known PSK): a position sent there is readable
   * by every Meshtastic radio in RF range, which is not what a hunting party
   * wants. Our own devices decode positions from any channel they share.
   *
   * ⚠ THIS AUTO-PICK IS LEFT EXACTLY AS IT SHIPPED, including its fall-through
   * to the public channel when there is no private one. It is a ONE-SHOT
   * gesture the user just made by hand, and it is not the path this feature
   * added. The periodic beacon deliberately does NOT share it: repeated
   * automatic sends are where a silent public fall-through stops being a
   * defensible default and becomes the worst failure this design can have, so
   * the beacon demands a named channel and refuses without one. */
  const MeshChannel* ch = &channels[0];
  for (int i = 0; i < channelCount; i++) {
    if (channels[i].keyLen != 16 ||
        memcmp(channels[i].key, meshDefaultKey(), 16) != 0) {
      ch = &channels[i];
      break;
    }
  }
  lastAnnounceOk = sendPositionOn(myPinLatI, myPinLonI, ch, "pin");
  return lastAnnounceOk;
#else
  lastAnnounceOk = myPinSet;
  return myPinSet;
#endif
}

/* ── The periodic GPS position beacon ──────────────────────────────────────
 * Settings live in NVS ("posint"/"poschan"/"pospub"), read once in setup().
 * Every default is the conservative one and every read states it explicitly,
 * so a phone upgrading from firmware that never had this feature comes up off,
 * aimed nowhere, behaviourally identical to before. */
void MeshtasticService::loadPosSettings() {
  Preferences p;
  p.begin("wpmesh", true);
  posIntervalSecs = p.getULong("posint", 0);          // 0 = off, the default
  memset(posChanName, 0, sizeof(posChanName));
  /* getBytes, not getString: a String here would be a heap allocation at boot
   * for 24 bytes that already have a home. Absent key returns 0 and leaves the
   * buffer alone, which the memset above has already made "unset". */
  const size_t got = p.getBytes("poschan", posChanName, sizeof(posChanName));
  if (got == 0 || got > sizeof(posChanName)) {
    posChanName[0] = '\0';
  }
  posChanName[sizeof(posChanName) - 1] = '\0';        // never trust flash to terminate
  posChanWasPublic = p.getBool("pospub", false);
  /* The GPS mirror, from the SAME key WiPhone.ino reads into gGpsNmea. Read
   * here rather than waiting to be told, so there is no boot-order dependency
   * between this service's setup() and the .ino's — both just read `gpsen`. */
  gpsEnabled = p.getBool("gpsen", false);
  p.end();
  posNextTxMs = 0;
  posSkipRuns = 0;
  posLastValid = false;
  if (posIntervalSecs) {
    log_e("POSITION BEACON: every %lus to '%s'%s", (unsigned long)posIntervalSecs,
          posChanName[0] ? posChanName : "(no channel)",
          posChanWasPublic ? " [chosen while PUBLIC]" : "");
  }
}

void MeshtasticService::setPosInterval(uint32_t secs) {
  posIntervalSecs = secs;
  /* 🔑 RE-ARM. The keypress that switches reporting on must NEVER transmit:
   * the first beacon is a full period away. setNeighborInterval does this for
   * politeness; here it is the difference between "I chose to be findable" and
   * "I brushed a D-pad and my location went out". */
  posNextTxMs = 0;
  posSkipRuns = 0;
  Preferences p;
  p.begin("wpmesh", false);
  p.putULong("posint", secs);
  p.end();
  log_e("POSITION BEACON %s (%lus)", secs ? "ARMED" : "OFF", (unsigned long)secs);
}

void MeshtasticService::setPosChannelName(const char* name) {
  memset(posChanName, 0, sizeof(posChanName));
  if (name && name[0]) {
    strlcpy(posChanName, name, sizeof(posChanName));
  }
  /* Record what the channel WAS when it was chosen. An UNRESOLVABLE name
   * records false, not true: false leaves the hard refusal ARMED, so a name
   * that later appears carrying the stock public key is caught rather than
   * waved through. Only a channel that is on the phone AND public right now
   * can set this — which is exactly the user who just double-pressed it. */
  const MeshChannel* ch = posChanName[0] ? getPosChannel() : NULL;
  posChanWasPublic = (ch != NULL) && channelIsPublic(ch);
  Preferences p;
  p.begin("wpmesh", false);
  p.putBytes("poschan", posChanName, sizeof(posChanName));
  p.putBool("pospub", posChanWasPublic);
  p.end();
  posNextTxMs = 0;        // retargeting waits a full period too
  posSkipRuns = 0;
  log_e("POSITION BEACON target '%s'%s", posChanName[0] ? posChanName : "(none)",
        posChanWasPublic ? " - PUBLIC, confirmed by the user" : "");
}

/* Resolved BY NAME, every time, never from a remembered index. applyChannelUrl()
 * can append or re-key channels under an open menu, and an index frozen when
 * the picker was drawn would aim a live location beacon at whatever slid into
 * that slot. A name that is no longer on the phone resolves to NULL, which the
 * beacon reads as "refuse to send" — the correct failure. */
const MeshChannel* MeshtasticService::getPosChannel() const {
  if (!posChanName[0]) {
    return NULL;
  }
  for (int i = 0; i < channelCount; i++) {
    if (strcmp(channels[i].name, posChanName) == 0) {
      return &channels[i];
    }
  }
  return NULL;
}

/* 🛑 THE HARD REFUSAL, and it is deliberately narrow. It catches the ACCIDENT —
 * someone applies a channel URL that re-keys 'hunt-group' to the stock default,
 * and a beacon armed months ago silently starts broadcasting a person's live
 * location in the clear. It does NOT override the user who deliberately
 * double-pressed LongFast in the picker, because for them pospub is true and a
 * confirm that produces nothing would be a lie by UI. */
static const char* const POS_REFUSED_PUBLIC = "REFUSED - that channel is public";

const char* MeshtasticService::posBlockedReason() const {
  /* Deliberately does NOT look at posIntervalSecs: the bench's forced send
   * must obey exactly these rules with reporting switched off, and one gate
   * used by both callers cannot drift out of step with itself. The UI asks
   * this only when an interval is set. */
  if (!gpsEnabled) {
    return "GPS is off - nothing sent";
  }
  if (!posChanName[0]) {
    return "no channel picked - nothing sent";
  }
  const MeshChannel* ch = getPosChannel();
  if (!ch) {
    return "channel is gone - nothing sent";
  }
  if (channelIsPublic(ch) && !posChanWasPublic) {
    return POS_REFUSED_PUBLIC;
  }
  /* MESH_POS_TX_FRESH_MS, not MESH_GPS_FRESH_MS: see the note on both. Spending
   * airtime to tell a hunting party where somebody is, from a receiver that has
   * missed 30 straight epochs, is a guess dressed as a measurement. */
  if (gpsFixMs == 0) {
    return "no fix yet - nothing sent";
  }
  /* ⚠ SPLIT FROM THE LINE ABOVE ON PURPOSE. These are different facts and a
   * walker needs to tell them apart: "never acquired" means check the antenna
   * and the sky; "acquired and lost" means keep walking, it was working 40
   * seconds ago. Reporting the second as the first sends people to debug
   * hardware that is fine. The Places GPS row already draws this distinction
   * ("no fix yet" vs "last fix N min ago") and this line was contradicting it. */
  if ((uint32_t)(millis() - gpsFixMs) >= MESH_POS_TX_FRESH_MS) {
    return "fix too old - nothing sent";
  }
  /* ── AND IT HAS TO BE A FIX WORTH BELIEVING ───────────────────────────────────────────
   *
   * 🛑 MEASURED 2026-08-25, and it is why this gate exists: WiPhone 2 indoors produced
   * `FIRST FIX 47.33821,-122.16501 sats=3 hdop=6.4` while the phone was in fact at
   * 47.4965,-122.3749 — **about 20 km out**. Nothing here would have refused it. Had that
   * landed on a beacon tick, COVEY's map would have shown Nick 20 km from where he was, with
   * no hint anything was wrong. Every other rule on this path exists to avoid "a confident
   * wrong dot on somebody else's map"; this was the hole in that argument.
   *
   * ⚠ THREE SATELLITES IS A 2D FIX. It solves for latitude and longitude by ASSUMING an
   * altitude, and when that assumption is wrong the error goes into the horizontal — which is
   * the one number a hunting party reads. Four is the arithmetic minimum for a real 3D fix,
   * so this is the standard bar, not a strict one: under canopy a working receiver tracks
   * five to ten.
   *
   * ⚠ REFUSES ONLY WHAT IT POSITIVELY KNOWS IS BAD. Both fields are -1 when the receiver has
   * not told us (they come from GGA), and an unknown is NOT treated as a failure — refusing on
   * silence would break any receiver that does not emit GGA, which is a different bug than the
   * one being fixed. */
  if (!meshPosFixUsable(gpsSats, gpsHdopX10)) {
    return (gpsSats >= 0 && gpsSats < MESH_POS_MIN_SATS)
               ? "2D fix only (under 4 satellites) - nothing sent"
               : "fix too imprecise (HDOP) - nothing sent";
  }
  return NULL;
}

bool MeshtasticService::sendGpsPosition() {
  posLastOk = false;
  const char* blocked = posBlockedReason();
  if (blocked) {
    log_e("POSITION BEACON %s: %s", blocked == POS_REFUSED_PUBLIC ? "REFUSED" : "SKIPPED",
          blocked);
    return false;
  }
  /* Re-resolved HERE, at send time. Everything above was checked against this
   * same call, microseconds ago and on the same task — nothing can move the
   * channel table in between. */
  const MeshChannel* ch = getPosChannel();
  const int32_t latI = gpsLatI, lonI = gpsLonI;
  const bool ok = sendPositionOn(latI, lonI, ch, "gps beacon");
  posLastTxMs = millis();
  posLastOk = ok;
  if (ok) {
    /* The movement gate measures from what actually WENT OUT, not from the last
     * fix: a failed send must not make the next slot think the mesh already
     * knows this position. */
    posLastLatI = latI;
    posLastLonI = lonI;
    posLastValid = true;
    posSkipRuns = 0;
  }
  return ok;
}

bool MeshtasticService::sendGpsPositionNow() {
  /* ⚠ THE SPACING GATE APPLIES TO THE FORCED SEND TOO. Without it, a `pos now`
   * issued just after a periodic rebroadcast puts two ~518 ms blocking transmits
   * back to back — a ~1.05 s frozen superloop, which is the precise thing
   * s_lastPhyTxMs was introduced to prevent. A bench command is still allowed to
   * jump the interval; it is not allowed to jump the airtime spacing. */
  if (s_lastPhyTxMs != 0 && (uint32_t)(millis() - s_lastPhyTxMs) < 1500UL) {
    log_e("POS: 'now' refused - a send finished <1.5 s ago, spacing it");
    return false;
  }
  return sendGpsPosition();
}

/* ── PKC key management ─────────────────────────────────────────────────────────────
 * The identity keypair lives in NVS next to the node name. It is generated ONCE and
 * must then be treated as PERMANENT: every peer stores our public key on first hearing
 * it and stock firmware NEVER overwrites a stored key — if this phone's key changes
 * (chip erase, NVS wipe), peers keep encrypting to the DEAD key and every DM fails,
 * silently, both ways, until the phone's node entry is deleted on each peer.
 * The serial `pki` command prints the key so that state can at least be SEEN. */
void MeshtasticService::loadOrCreatePkiKeys() {
  Preferences prefs;
  prefs.begin("wpmesh", false);
  size_t got = prefs.getBytes("pkipriv", myPkiPriv, sizeof(myPkiPriv));

  uint8_t acc = 0;
  for (size_t i = 0; i < sizeof(myPkiPriv); i++) {
    acc |= myPkiPriv[i];
  }
  if (got != sizeof(myPkiPriv) || acc == 0) {
    // First boot (or wiped NVS): make a keypair. esp_random is the RF-seeded HWRNG.
    for (int i = 0; i < 8; i++) {
      uint32_t r = esp_random();
      memcpy(myPkiPriv + i * 4, &r, 4);
    }
    meshPkiClampPrivate(myPkiPriv);
    prefs.putBytes("pkipriv", myPkiPriv, sizeof(myPkiPriv));
    log_e("MESH PKI: NEW keypair generated - peers that stored an old key for "
          "!%08x must delete this node to DM again", (unsigned)myNodeNum);
  }

  /* The public key is always recomputed from the private one (~tens of ms, boot
   * only) — the stored copy exists purely so `pki` can spot NVS corruption. */
  meshPkiPublicKey(myPkiPub, myPkiPriv);
  uint8_t storedPub[MESH_PKI_KEY_LEN];
  if (prefs.getBytes("pkipub", storedPub, sizeof(storedPub)) != sizeof(storedPub) ||
      memcmp(storedPub, myPkiPub, sizeof(storedPub)) != 0) {
    prefs.putBytes("pkipub", myPkiPub, sizeof(myPkiPub));
  }
  prefs.end();
  pkiReady = true;
}

bool MeshtasticService::pkiKeyCached(uint32_t nodeNum, uint8_t keyOut[MESH_KEY_LEN]) const {
  for (int i = 0; i < 2; i++) {
    if (pkiCache[i].node == nodeNum && nodeNum != 0) {
      memcpy(keyOut, pkiCache[i].key, MESH_KEY_LEN);
      return true;
    }
  }
  return false;
}

/* Derive (or fetch) the AES session key for a node. ⚠ SUPERLOOP DEPTH ONLY on a
 * cache miss: the X25519 underneath costs ~3 KB of transient stack. GUI-depth
 * callers must use pkiKeyCached() and queue on a miss (sendDirectMessage does). */
bool MeshtasticService::pkiKeyForNode(const MeshNode* n, uint8_t keyOut[MESH_KEY_LEN]) {
  if (!n || !(n->pkiFlags & MESH_NODE_HAS_KEY) || !pkiReady) {
    return false;
  }
  if (pkiKeyCached(n->nodeNum, keyOut)) {
    return true;
  }
  if (!meshPkiDeriveKey(myPkiPriv, n->pubKey, keyOut)) {
    log_e("MESH PKI: key derive FAILED for !%08x (weak/zero key)", (unsigned)n->nodeNum);
    return false;
  }
  PkiCacheEntry& slot = pkiCache[pkiCacheNext];
  pkiCacheNext = (uint8_t)((pkiCacheNext + 1) % 2);
  slot.node = n->nodeNum;
  memcpy(slot.key, keyOut, MESH_KEY_LEN);
  return true;
}

/* Trust-on-first-use, matching stock: the FIRST key heard for a node sticks; a
 * later different key is flagged and ignored (a real re-key on the peer needs
 * "Clear nodes" here — the flag is visible in the serial `pki` command). */
void MeshtasticService::pkiLearnKey(MeshNode* n, const uint8_t* key) {
  if (!n || !key) {
    return;
  }
  if (n->pkiFlags & MESH_NODE_HAS_KEY) {
    if (memcmp(n->pubKey, key, MESH_KEY_LEN) != 0 &&
        !(n->pkiFlags & MESH_NODE_KEY_MISMATCH)) {
      n->pkiFlags |= MESH_NODE_KEY_MISMATCH;
      dbDirty = true;
      log_e("MESH PKI: !%08x announced a DIFFERENT key - KEPT the first one. "
            "If the node really re-keyed, Clear nodes to trust the new key.",
            (unsigned)n->nodeNum);
    }
    return;
  }
  memcpy(n->pubKey, key, MESH_KEY_LEN);
  n->pkiFlags |= MESH_NODE_HAS_KEY;
  dbDirty = true;
  /* A cached session key may exist from a PREVIOUS life of this node number
   * (evicted from the table, came back re-keyed): the fresh learn must not
   * leave a derive from the old pubkey answering for the new one. */
  for (int i = 0; i < 2; i++) {
    if (pkiCache[i].node == n->nodeNum) {
      memset(&pkiCache[i], 0, sizeof(pkiCache[i]));
    }
  }
  log_e("MESH PKI: learned key for !%08x (%02x%02x%02x%02x...) - DMs unlocked",
        (unsigned)n->nodeNum, key[0], key[1], key[2], key[3]);
  /* Called from the RX path (superloop depth): warm the cache now so the
   * user's first DM back is a cache hit even from GUI depth. */
  uint8_t k[MESH_KEY_LEN];
  (void)pkiKeyForNode(n, k);
}

// Conversation id for a message: 0 = broadcast (Main Channel), else the DM peer.
uint32_t MeshtasticService::chatKeyOf(uint32_t from, uint32_t to) const {
  if (to == MESH_BROADCAST_ADDR) {
    return 0;
  }
  return (from == myNodeNum) ? to : from;
}

void MeshtasticService::removeMessageAt(int idx) {
  if (idx < 0 || idx >= msgCount) {
    return;
  }
  if (idx < msgCount - 1) {
    memmove(&messages[idx], &messages[idx + 1],
            (msgCount - 1 - idx) * sizeof(MeshMessage));
  }
  msgCount--;
}

void MeshtasticService::deriveShortName() {
  // Short name = up to 4 characters of the long name (Meshtastic convention).
  int j = 0;
  for (int i = 0; myLongName[i] && j < 4; i++) {
    if (myLongName[i] != ' ') {
      myShortName[j++] = myLongName[i];
    }
  }
  if (j == 0) {
    snprintf(myShortName, sizeof(myShortName), "W%03x", (unsigned)(myNodeNum & 0xFFF));
  } else {
    myShortName[j] = '\0';
  }
}

void MeshtasticService::loadMyName() {
  Preferences prefs;
  prefs.begin("wpmesh", true);                 // read-only
  String ln = prefs.getString("lname", "");
  String sn = prefs.getString("sname", "");
  myHopLimit = (uint8_t)prefs.getUChar("hoplim", 3);
  prefs.end();
  if (ln.length() > 0) {
    strlcpy(myLongName, ln.c_str(), sizeof(myLongName));
  } else {
    snprintf(myLongName, sizeof(myLongName), "WiPhone-%04x", (unsigned)(myNodeNum & 0xFFFF));
  }
  if (myHopLimit < 1 || myHopLimit > 7) {
    myHopLimit = 3;
  }
  s_hopLimit = myHopLimit;

  /* An explicitly chosen short name wins and must NOT be overwritten by deriveShortName()
   * the next time the long name changes — that is the whole point of setting one. */
  if (sn.length() > 0) {
    shortNameCustom = true;
    strlcpy(myShortName, sn.c_str(), sizeof(myShortName));
  } else {
    shortNameCustom = false;
    deriveShortName();
  }
}

void MeshtasticService::setMyShortName(const char* shortName) {
  Preferences prefs;
  prefs.begin("wpmesh", false);                // read-write

  if (!shortName || !shortName[0]) {
    // Empty means "go back to following the long name".
    shortNameCustom = false;
    prefs.remove("sname");
    deriveShortName();
  } else {
    /* Meshtastic short names are 4 characters by convention — that is what other clients
     * lay out for in their node lists. Trim rather than refuse, so a long entry still does
     * something sensible instead of silently failing. */
    char buf[MESH_SHORT_NAME_MAX + 1];
    strlcpy(buf, shortName, sizeof(buf));
    shortNameCustom = true;
    strlcpy(myShortName, buf, sizeof(myShortName));
    prefs.putString("sname", myShortName);
  }
  prefs.end();

  lastNodeInfoTxMs = millis();
  announceNodeInfo(true);                      // let the mesh learn the new short name
  log_i("Mesh short name is now '%s' (%s)", myShortName,
        shortNameCustom ? "custom" : "derived from long name");
}

void MeshtasticService::setHopLimit(uint8_t hops) {
  if (hops < 1) hops = 1;
  if (hops > 7) hops = 7;
  myHopLimit = hops;
  s_hopLimit = hops;
  Preferences prefs;
  prefs.begin("wpmesh", false);
  prefs.putUChar("hoplim", hops);
  prefs.end();
  log_i("Mesh: hop limit set to %u", hops);
}

void MeshtasticService::setMyName(const char* longName) {
  if (!longName || !longName[0]) {
    return;
  }
  strlcpy(myLongName, longName, sizeof(myLongName));
  if (!shortNameCustom) {
    deriveShortName();     // only when the user has not chosen one; theirs must survive
  }

  Preferences prefs;
  prefs.begin("wpmesh", false);                // read-write
  prefs.putString("lname", myLongName);
  prefs.end();

  lastNodeInfoTxMs = millis();
  announceNodeInfo(true);                       // let the mesh learn the new name
  log_i("Mesh node renamed to '%s' (%s)", myLongName, myShortName);
}

void MeshtasticService::announceNodeInfo(bool wantResponse) {
#ifdef MESHTASTIC_PHY
  /* ⚠ log_e ON PURPOSE, not log_i. Only log_e is compiled into this build, so an log_i here
   * is invisible — which is precisely why "does this phone announce itself?" went unanswered
   * for so long. COVEY holds 80 nodes and none of them is this phone, so every fact below is
   * needed to tell "never transmitted" from "transmitted and ignored". */
  if (radioState != MESH_RADIO_READY) {
    log_e("MESH ANNOUNCE SKIPPED: radio not ready (state=%d)", (int)radioState);
    return;
  }
  if (channelCount <= 0) {
    log_e("MESH ANNOUNCE SKIPPED: no channels configured");
    return;
  }
  const bool ok = meshTxNodeInfo(myNodeNum, myLongName, myShortName, wantResponse, &channels[0],
                                 pkiReady ? myPkiPub : NULL);
  log_e("MESH ANNOUNCE %s: node=!%08x '%s' (%s) ch='%s' hash=0x%02X keyLen=%u wantResp=%d pki=%s",
        ok ? "SENT" : "FAILED", (unsigned)myNodeNum, myLongName, myShortName,
        channels[0].name, channels[0].hash, (unsigned)channels[0].keyLen, (int)wantResponse,
        pkiReady ? "in packet" : "MISSING");
#else
  (void)wantResponse;
#endif
}

void MeshtasticService::clearMessages() {
  msgCount = 0;
  dbDirty = true;
  saveDb();                                   // persist immediately
  log_i("Mesh: messages cleared");
}

void MeshtasticService::clearNodes() {
  nodeCount = 0;
  if (nodes) {
    memset(nodes, 0, (size_t)MESH_MAX_NODES * sizeof(MeshNode));   // NOT sizeof(nodes)
  }
  upsertNode(myNodeNum, myLongName);          // keep this node in the list
  /* Cached session keys are derived from the stored pubkeys that were just
   * wiped — a stale entry here would keep "trusting" a key the user explicitly
   * cleared (this is the re-TOFU path after a peer re-keys). */
  pkiCacheClear();
  dbDirty = true;
  saveDb();
  log_i("Mesh: node DB cleared");
}

void MeshtasticService::scheduleRebroadcast(const uint8_t* pkt, uint8_t len) {
  if (len == 0 || len > sizeof(rebroadcast[0].data)) {
    return;
  }
  for (int i = 0; i < 4; i++) {
    if (!rebroadcast[i].active) {
      memcpy(rebroadcast[i].data, pkt, len);
      rebroadcast[i].len = len;
      // 130..700 ms random jitter so relays don't all transmit at once.
      rebroadcast[i].dueMs = millis() + 130 + (esp_random() % 570);
      rebroadcast[i].active = true;
      return;
    }
  }
}

bool MeshtasticService::seenPacketId(uint32_t id) {
  if (id == 0) {
    return false;
  }
  for (int i = 0; i < 16; i++) {
    if (recentPktIds[i] == id) {
      return true;
    }
  }
  recentPktIds[recentPktPos] = id;
  recentPktPos = (recentPktPos + 1) % 16;
  return false;
}

void MeshtasticService::setup() {
  if (initialized) {
    return;
  }
  initialized = true;

  myNodeNum = chipId;

  /* The node table moved OUT of internal RAM (it was a plain member array in BSS). Unlike the
   * replay ring this DOES fall back to internal on a PSRAM failure: messages and replay are
   * luxuries, but a phone with no node table cannot name a sender or DM anybody. */
  nodes = (MeshNode*)ps_malloc((size_t)MESH_MAX_NODES * sizeof(MeshNode));
  if (!nodes) {
    nodes = (MeshNode*)malloc((size_t)MESH_MAX_NODES * sizeof(MeshNode));
  }
  nodeOrder = (uint16_t*)ps_malloc((size_t)MESH_MAX_NODES * sizeof(uint16_t));
  if (!nodeOrder) {
    nodeOrder = (uint16_t*)malloc((size_t)MESH_MAX_NODES * sizeof(uint16_t));
  }
  if (nodes) {
    memset(nodes, 0, (size_t)MESH_MAX_NODES * sizeof(MeshNode));
  } else {
    log_e("MeshtasticService: NODE TABLE ALLOC FAILED");
  }
  nodeCount = 0;
  orderDirty = true;

  // Allocate the message store in PSRAM (falls back to internal RAM if needed).
  messages = (MeshMessage*)ps_malloc((size_t)MESH_MSG_CAP * sizeof(MeshMessage));
  if (!messages) {
    messages = (MeshMessage*)malloc((size_t)MESH_MSG_CAP * sizeof(MeshMessage));
  }
  msgCount = 0;
  if (!messages) {
    log_e("MeshtasticService: message buffer alloc FAILED");
  }

  // Replay ring + reply slab — PSRAM only, no internal fallback: history
  // replay is a luxury and must never compete with SIP/WiFi for real RAM.
  replayRing = (ReplayHeard*)ps_malloc((size_t)REPLAY_RING_CAP * sizeof(ReplayHeard));
  replayPkts = (char*)ps_malloc((size_t)REPLAY_MAX_PKTS * REPLAY_PKT_STRIDE);
  nbrCount = 0;
  nbrNextTxMs = 0;
  nbrPendingMask = 0;
  nbrDripMs = 0;
  nbrLastTxMs = 0;
  nbrLastTxCount = 0;
  {
    Preferences p;
    p.begin("wpmesh", true);
    nbrIntervalSecs = p.getULong("nbrint", 0);   // 0 = off, the default
    p.end();
  }
  replayHead = replayCount = 0;
  replayPktCount = replayPktNext = 0;
  replayNextTxMs = 0;
  replayCoverFrom = 0;
  replayChanHash = 0;
  replayServedMs = 0;
  replayServedN = 0;
  if (!replayRing || !replayPkts) {
    log_e("MeshtasticService: replay buffers alloc FAILED - replay inert");
    replayRing = NULL;                 // both or neither; keeps the checks single
  }

  // Load our editable node name (from NVS, or a default derived from the id).
  loadMyName();

  // PKC identity: load or mint the X25519 keypair (NVS). Announced in NodeInfo.
  loadOrCreatePkiKeys();

  // The user-declared position pin + distance reference (NVS).
  loadPin();

  /* The GPS position beacon's settings (NVS). Independent of loadPin() above —
   * they share no state, and deliberately so: nothing on the beacon path reads
   * or writes the pin. Comes up off and aimed nowhere unless a person
   * previously said otherwise. */
  loadPosSettings();

  // Channels: start with LongFast, then restore any saved custom channels.
  initDefaultChannel();
  loadChannels();

  // Restore persisted nodes + message history (SPIFFS is mounted by now).
  loadFavourites();      // before loadDb: restored nodes get their star stamped on
  loadDb();

  // Always register ourselves as a node, under our own name.
  upsertNode(myNodeNum, myLongName);

  /* Pre-warm the session-key cache for the most-recently-heard keyed nodes so
   * the first DM after boot is a cache hit even from GUI depth (the derive
   * needs ~3 KB of stack and belongs here, where the stack is shallow). */
  {
    int warmed = 0;
    uint8_t k[MESH_KEY_LEN];
    for (int i = 0; i < nodeCount && warmed < 2; i++) {
      if ((nodes[i].pkiFlags & MESH_NODE_HAS_KEY) && nodes[i].nodeNum != myNodeNum) {
        if (pkiKeyForNode(&nodes[i], k)) {
          warmed++;
        }
      }
    }
  }

#ifdef MESHTASTIC_PHY
  // Real radio: bring up the SX1276 in Meshtastic LongFast RX mode.
  /* ⚠ Identity banner at log_e so it actually appears — only log_e is compiled in.
   * ⚠ myNodeNum comes from the LEGACY WiPhone `chipId`, which is built from just THREE MAC
   * bytes (WiPhone.ino: `for(i=0; i<17; i+=8)`), so it is a 24-bit value and always prints
   * as !00xxxxxx. Every one of the 80 nodes in COVEY's database has a non-zero top byte,
   * because stock firmware uses the full 4-byte MAC tail. Whether that is merely unusual or
   * is why nothing lists this phone is UNPROVEN — but it is the first thing to check against
   * a receiving node, and it cannot be checked without printing it. */
  log_e("MESH IDENTITY: node=!%08x (%u-bit) long='%s' short='%s' hop=%u pki=%02x%02x%02x%02x...",
        (unsigned)myNodeNum, myNodeNum > 0xFFFFFF ? 32 : 24,
        myLongName, myShortName, (unsigned)myHopLimit,
        myPkiPub[0], myPkiPub[1], myPkiPub[2], myPkiPub[3]);
  /* Does this phone HEAR the mesh? The node DB is restored from SPIFFS just above, so a
   * healthy count here means RX has been working and the radio parameters match the mesh
   * exactly — which would leave the packet CONTENT as the only reason nothing lists us.
   * A count of 1 (just ourselves) would mean the radio never hears anything either. */
  log_e("MESH DB: %d nodes, %d messages stored", nodeCount, msgCount);
  for (int i = 0; i < nodeCount && i < 6; i++) {
    log_e("MESH DB node[%d]: !%08x '%s'", i, (unsigned)nodes[i].nodeNum, nodes[i].name);
  }
  for (int i = 0; i < channelCount; i++) {
    log_e("MESH CHANNEL[%d]: '%s' hash=0x%02X keyLen=%u", i, channels[i].name,
          channels[i].hash, (unsigned)channels[i].keyLen);
  }

  if (meshPhy.begin()) {
    radioState = MESH_RADIO_READY;
    log_e("MESH RADIO READY: %lu Hz", (unsigned long)meshPhy.getFrequencyHz());
    log_i("MeshtasticService: radio READY, node=0x%08X (%s), ch=%s, freq=%lu",
          myNodeNum, myLongName, channelName, (unsigned long)meshPhy.getFrequencyHz());
    // Announce ourselves and solicit other nodes' NodeInfo so names populate.
    announceNodeInfo(true);
  } else {
    radioState = MESH_RADIO_ERROR;
    log_e("MeshtasticService: radio init FAILED");
  }
#else
  // Stubbed radio: mark state and seed demo data so the UI is not empty.
  radioState = MESH_RADIO_STUBBED;
  seedStubData();
  log_i("MeshtasticService: stub radio ready, node=0x%08X, ch=%s", myNodeNum, channelName);
#endif
}

bool MeshtasticService::loop() {
  // Debounced persistence of new nodes/messages (limits flash wear).
  /* Starred IDs are tiny and are written on the very next tick rather than on the DB's
   * debounce: the file is a few dozen bytes, and a star the user just set should survive a
   * battery pull a second later. It is off the input path, which is the part that matters. */
  if (favDirty) {
    favDirty = false;
    saveFavourites();
  }
  /* ⚠ `uiIdle` is the third condition and it is not optional. A save blocks this task for
   * ~1.5 s (MEASURED: 6.7 KB in ~1200 ms — SPIFFS on this part runs about 6 KB/s, and the
   * filesystem is only 2.6% full, so it is the flash, not fragmentation). Anything that
   * blocks here blocks the keypad, the screen and the WiFi stack together, which is why the
   * symptom was "menus freeze for a second or two AND WiFi drops". Deferring until nobody is
   * touching the phone does not make the save faster — it makes it land where it cannot be
   * felt. The data still persists within seconds of the user putting the phone down. */
  /* Drain any save in flight FIRST, and unconditionally: each step is bounded (128 bytes,
   * ~20 ms) so it cannot freeze anything, and letting it finish promptly keeps the temp file's
   * lifetime short. Only STARTING a save waits for an idle moment. */
  /* s_meshCardIn is set by setCardPresent(), which the main loop calls every pass AND once
   * before setup() — it is not synced here any more. One writer: two of them was how the
   * load path came to disagree with the save path for a day and a half. */
  saveDbStep();

  if (dbDirty && uiIdle && !saveActive && (millis() - lastSaveMs > MESH_SAVE_DEBOUNCE_MS)) {
    saveDb();
    dbDirty = false;
    lastSaveMs = millis();
  }

  /* Places age out on their OWN clock, not only when a packet mentions them:
   * a pin set on COVEY to die in a day dies here in a day too. Once a minute
   * is plenty — expiries are set in hours. Needs the real clock; guessing
   * with an unset clock could throw camp away at boot. */
  if (ntpClock.isTimeKnown() && (int32_t)(millis() - nextWpSweepMs) >= 0) {
    nextWpSweepMs = millis() + 60000;
    /* UTC: MeshWaypoint::expire came off the air from COVEY and is real UTC, so comparing it
     * against the local-shifted epoch made every pin outlive its own deadline by the timezone
     * offset — eight hours here. */
    uint32_t nowU = (uint32_t)ntpClock.getExactUtcTime();
    for (int i = waypointCount - 1; i >= 0; i--) {
      if (waypoints[i].expire != 0 && nowU > waypoints[i].expire) {
        log_e("MESH WAYPOINT: '%s' expired - removed", waypoints[i].name);
        waypoints[i] = waypoints[--waypointCount];
        dbDirty = true;
        placesNews = true;
      }
    }
  }

#ifdef MESHTASTIC_PHY
  /* ── IS THE RADIO STILL THERE? (woods plate, 2026-08-22) ─────────────────────────
   * The plate's radio lives on the EXTERNAL pack, and the phone can outlive it by
   * hours. Measured failure: with the pack dead the phone's own driven pins phantom-
   * powered the RFM95W through its ESD clamps (rail floated at 2.54 V) - the chip
   * answered its version probe, this state machine said READY, and every "sent"
   * message went into a radio that could never transmit. R3-R6 (1 k series) make the
   * rail collapse instead; this check makes the firmware notice and SAY so.
   *
   * healthCheck() is version + op-mode, not version alone: a radio whose pack came
   * BACK answers 0x12 from POR defaults (FSK standby, LoRa bit clear) - present,
   * deaf, and mute. Op-mode catches it, and the reinit path below re-runs the whole
   * config so a swapped pack recovers in <=10 s with no reboot. Costs two register
   * reads (~240 us of bit-bang) every 5 s. */
  {
    static uint32_t nextHealthMs = 0;
    static uint32_t failCount = 0;      // consecutive reinit failures; paces the retry
    const uint32_t nowH = millis();
    if ((int32_t)(nowH - nextHealthMs) >= 0) {
      if (radioState == MESH_RADIO_READY) {
        nextHealthMs = nowH + 5000;
        if (!meshPhy.healthCheck()) {
          radioState = MESH_RADIO_ERROR;
          log_e("MESH RADIO LOST (pack dead or radio reset) - sends will refuse honestly");
        }
      } else if (radioState == MESH_RADIO_ERROR) {
        /* Retry fast while a swapped pack is plausible, then slow down. A phone with
         * NO daughterboard fitted fails this forever, and reinit() blocks the shared
         * loop task for 10 ms each time (delay() yields, so it costs no current - but
         * it stalls audio and SIP pumping once every 10 s for the life of the boot).
         * Ten seconds for the first two minutes covers every real pack swap; after
         * that a minute is plenty, and a fitted plate still recovers inside one. */
        nextHealthMs = nowH + (failCount < 12 ? 10000 : 60000);
        /* Log the failure on the first attempt and then every 30th. A phone with
         * no plate fitted fails this for as long as it is switched on, and an
         * error line per retry buries every other diagnostic in the log —
         * measured on the bench the day this shipped. With the backoff above,
         * every 30th works out at roughly half-hourly once it has settled, which
         * is the right cadence for "still no radio" while recovery stays fast. */
        const bool speak = (failCount % 30) == 0;
        if (meshPhy.reinit(speak)) {
          failCount = 0;
          radioState = MESH_RADIO_READY;
          log_e("MESH RADIO RECOVERED - reconfigured, announcing");
          announceNodeInfo(true);   // same ask-the-mesh announce as boot
        } else {
          failCount++;
        }
      } else {
        nextHealthMs = nowH + 10000;
      }
    }
  }

  /* ── RE-ANNOUNCE ON A TIMER, LIKE EVERY OTHER NODE ────────────────────────────────
   * The phone used to announce its NodeInfo exactly three times ever: once at boot, once
   * when the name was edited, and once if you pressed the button in the Meshtastic app.
   * Discovery on a LoRa mesh is PASSIVE — a node is learned only when it transmits
   * something you can hear — so any radio that booted, cleared its node DB, or came into
   * range AFTER our boot announce would list the phone as a bare node number with no name,
   * possibly forever.
   *
   * Stock firmware re-announces every node_info_broadcast_secs, which defaults to 3 hours;
   * MESH_NODEINFO_PERIOD_MS matches that so the phone behaves like the devices around it.
   *
   * ⚠ wantResponse=FALSE. A periodic beacon that also demanded everyone else reply would
   * turn one node's housekeeping into a mesh-wide storm every three hours.
   *
   * The first one fires a period after boot, since setup() already announced. */
  if (radioState == MESH_RADIO_READY && channelCount > 0) {
    const uint32_t now = millis();
    if (nextNodeInfoMs == 0) {
      nextNodeInfoMs = now + MESH_NODEINFO_PERIOD_MS;
    } else if ((int32_t)(now - nextNodeInfoMs) >= 0) {
      nextNodeInfoMs = now + MESH_NODEINFO_PERIOD_MS;
      lastNodeInfoTxMs = now;
      announceNodeInfo(false);
      log_i("Mesh: periodic NodeInfo as '%s' (%s)", myLongName, myShortName);
    }
  }

  /* Neighbour announce, if switched on. Same shape as the NodeInfo beacon
   * above: one packet, no want_response — asking every hearer to answer would
   * turn a map update into a storm. */
  if (radioState == MESH_RADIO_READY && nbrIntervalSecs > 0 && channelCount > 0) {
    const uint32_t now = millis();
    if (nbrNextTxMs == 0) {
      nbrNextTxMs = now + nbrIntervalSecs * 1000UL;   // first one a full period in
    } else if ((int32_t)(now - nbrNextTxMs) >= 0) {
      nbrNextTxMs = now + nbrIntervalSecs * 1000UL;
      /* Arm EVERY private channel — Nick runs more than one ('Howe group' and
       * a hunt group), and each has its own membership, so each needs the map.
       * Never the primary: index 0 is stock LongFast with the public key. */
      nbrPendingMask = 0;
      for (int i = 1; i < channelCount && i < 8; i++) {
        if (channels[i].keyLen > 0 && !channelIsMachine(&channels[i])) {
          nbrPendingMask |= (uint8_t)(1u << i);
        }
      }
      if (nbrPendingMask == 0) {
        log_e("NEIGHBOR: no private channel - not announcing (the primary is public)");
      }
      nbrDripMs = now;
    }
    /* ONE channel per pass, spaced: meshPhy.send() BLOCKS, and a LongFast
     * packet is most of a second on air. Three back-to-back sends would stall
     * the superloop long enough to matter (the task watchdog is not
     * theoretical on this phone). */
    if (nbrPendingMask && (int32_t)(now - nbrDripMs) >= 0) {
      for (int i = 1; i < channelCount && i < 8; i++) {
        if (nbrPendingMask & (1u << i)) {
          nbrPendingMask &= (uint8_t)~(1u << i);
          announceNeighborsOn(&channels[i]);
          break;
        }
      }
      nbrDripMs = now + 2000;
    }
  }

  /* ── PERIODIC GPS POSITION BEACON ─────────────────────────────────────────
   * Same shape as the two beacons above and deliberately not a second timing
   * mechanism: signed-difference deadline (millis-rollover safe), a
   * posNextTxMs == 0 "not armed yet" sentinel so a fresh choice waits a full
   * period, and the deadline recomputed AT FIRE TIME so a period spent inside
   * the Game Boy emulator (which skips this whole loop) costs one beacon
   * rather than a backlog burst.
   *
   * Three extra terms, all about not being a nuisance on a shared band:
   *
   *  1. posIntervalSecs > 0 is the arming switch. Default 0. Persisted.
   *
   *  2. 🛑 NEVER TWO TRANSMITS IN ONE PASS. meshPhy.send() blocks for the whole
   *     airtime — 518 ms for this 37-byte frame at the LongFast registers —
   *     and the rebroadcast queue, the replay pump and the neighbour drip can
   *     all fire in the same iteration. Two back to back is ~1.05 s of frozen
   *     keypad and GUI. When the spacing gate fails the deadline is
   *     deliberately NOT advanced: the slot is retried next pass, not lost.
   *
   *  3. The movement gate (meshPosShouldBeacon — pure, proven on the host by
   *     tests/test_pos.cpp): a phone that has not moved 100 m skips the slot,
   *     up to MESH_POS_MAX_SKIPS in a row so it can never go quiet for hours.
   *     That is 4x fewer transmissions while parked — arithmetic from the skip
   *     cap, NOT measured on hardware. It is checked here rather than inside
   *     sendGpsPosition() so the bench's forced `pos now` is not subject to it.
   *
   * 🛑 Nothing on this path calls delay(), waits on a hardware flag, takes a
   * queue or a semaphore, or changes the CPU frequency. The last of those is
   * the 09dbbda deadlock and it must stay unreachable from here. */
  if (radioState == MESH_RADIO_READY && posIntervalSecs > 0 && channelCount > 0) {
    const uint32_t now = millis();
    if (posNextTxMs == 0) {
      posNextTxMs = now + posIntervalSecs * 1000UL;   // first one a full period in
    } else if ((int32_t)(now - posNextTxMs) >= 0) {
      /* ⚠ THE SLOT IS NOW OWED, AND THE DEADLINE ADVANCES EITHER WAY. Whether there is a fix
       * to send decides WHEN it goes, not WHETHER the slot happened. */
      posNextTxMs = now + posIntervalSecs * 1000UL;
      posDue = true;
    }

    /* ── WHY A SLOT IS OWED RATHER THAN SPENT ─────────────────────────────────────────────
     *
     * 🛑 THIS USED TO SEND OR LOSE THE SLOT IN THE SAME INSTANT, and the two clocks involved
     * make that a bad bet: the interval is 300 s and MESH_POS_TX_FRESH_MS is 30 s. With a
     * solid fix that is fine — the fix refreshes every second, so every tick sends. But under
     * canopy, where the fix comes and goes, the odds that it happens to be under 30 s old at
     * the exact instant a 5-minute tick lands are poor, so a phone that HAD a perfectly good
     * fix forty seconds ago says nothing for another five minutes. That is the hunt scenario
     * (Nick, 2026-08-25).
     *
     * 🔑 The freshness rule is NOT relaxed by any of this — nothing older than 30 s is ever
     * transmitted, which was the whole point of it. What changes is that the slot waits for a
     * fresh fix instead of being thrown away by the absence of one. "At most this often"
     * rather than "only at these instants".
     *
     * ⚠ ONE SLOT MAX. `posDue` is a flag, not a counter, and the deadline keeps advancing
     * while it is set — so an hour with no sky owes exactly one beacon, not twelve. That is
     * the same anti-backlog rule the deadline recomputation above already follows. */
    if (posDue && (uint32_t)(now - s_lastPhyTxMs) >= 1500u) {
      const bool fresh = gpsEnabled && gpsFixMs != 0 &&
                         (uint32_t)(now - gpsFixMs) < MESH_POS_TX_FRESH_MS;
      if (fresh) {
        if (!meshPosShouldBeacon(posLastValid, posLastLatI, posLastLonI,
                                 gpsLatI, gpsLonI, posSkipRuns)) {
          posSkipRuns++;          // parked: this slot costs the band nothing
        } else {
          /* sendGpsPosition() re-checks EVERY safety rule itself (receiver on,
           * channel named, channel still here, channel not silently public,
           * fix fresh enough and good enough to be worth other people's trust). */
          sendGpsPosition();
        }
        /* Resolved either way. ⚠ Cleared even when sendGpsPosition() REFUSES, because the
         * reasons it can still refuse here (channel gone, channel silently public, fix
         * quality) are not things another pass will fix — retrying every pass would only
         * fill the log. Freshness is the one blocker that waits, and it is handled above. */
        posDue = false;
      }
      /* else: still owed. Try again next pass, and every pass, until a fix arrives. */
    }
  }

  /* Drain DMs that sendDirectMessage queued because their session key wasn't
   * cached (GUI depth cannot run the ~3 KB-stack X25519 derive; here it can).
   * One per tick; the local echo already happened at queue time. */
  for (int i = 0; i < 2; i++) {
    if (pendingDm[i].active) {
      pendingDm[i].active = false;
      const MeshNode* peer = findNode(pendingDm[i].dest);
      uint8_t skey[MESH_KEY_LEN];
      if (peer && pkiKeyForNode(peer, skey)) {
        if (!meshTxDataPki(myNodeNum, pendingDm[i].dest, MESH_PORT_TEXT_MESSAGE,
                           (const uint8_t*)pendingDm[i].text,
                           strlen(pendingDm[i].text), skey, 0, true)) {
          log_e("MESH DM to !%08x: queued PKI send FAILED", (unsigned)pendingDm[i].dest);
          /* It was echoed into the thread at queue time and never reached the air. Without
           * this it would sit there looking exactly like a message that went out fine. */
          setMessageReceipt(pendingDm[i].msgTimeMs, false, 4);
        } else {
          notePendingAck(s_lastTxPacketId, pendingDm[i].msgTimeMs);
        }
      } else {
        log_e("MESH DM to !%08x: key derive failed - message NOT sent",
              (unsigned)pendingDm[i].dest);
        setMessageReceipt(pendingDm[i].msgTimeMs, false, 4);
      }
      break;
    }
  }

  // Flood routing: send at most one due rebroadcast per tick (send blocks).
  for (int i = 0; i < 4; i++) {
    if (rebroadcast[i].active && (int32_t)(millis() - rebroadcast[i].dueMs) >= 0) {
      /* Bracketed because a rebroadcast is the most frequent thing this phone does that
       * nobody logs, and it is a candidate for the residual drift. Silent unless it moved
       * `largest` by 256 bytes or more — a quiet TX costs nothing to instrument. */
      const uint32_t before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      meshPhy.send(rebroadcast[i].data, rebroadcast[i].len);
      heapDelta("mesh-relay", before);
      rebroadcast[i].active = false;
      break;
    }
  }

  replayPump();   // drip one queued replay packet per gap (airtime politeness)

  uint8_t buf[MESH_PHY_MAX_PAYLOAD];
  uint8_t len = 0;
  int16_t rssi = 0;
  int8_t  snr = 0;
  if (!meshPhy.poll(buf, sizeof(buf), &len, &rssi, &snr)) {
    return false;
  }
  if (len < MESH_HEADER_LEN) {
    return false;                          // too short to be a Meshtastic packet
  }

  MeshPacketHeader hdr;
  memcpy(&hdr, buf, MESH_HEADER_LEN);
  uint8_t payloadLen = len - MESH_HEADER_LEN;

  /* Ignore packets we originated that the mesh rebroadcast back to us — but READ THE
   * RECEIPT OFF THEM FIRST.
   *
   * ⚠ A BROADCAST IS NEVER ACKNOWLEDGED BY A ROUTING PACKET. Meshtastic acknowledges a
   * broadcast IMPLICITLY: hearing your own packet rebroadcast is the acknowledgement, and
   * it proves the message entered the mesh — nothing about who received it. LoRa is
   * half-duplex so we cannot hear our own transmission; a packet arriving with our node
   * number on it has been through somebody else's radio by definition.
   *
   * This costs NOTHING on the air. Flood routing rebroadcasts any packet with hops left
   * whether or not want_ack is set, so broadcasts deliberately do NOT ask for an ack — the
   * bit would add no information and stock may answer it with a real routing packet. Only
   * DMs set want_ack, where the destination itself replies and "delivered" means it. */
  if (hdr.sender == myNodeNum) {
    /* ⚠ BROADCASTS ONLY. A DM floods too, so we hear our own DMs rebroadcast by neighbours
     * exactly the same way — and treating that as delivery would mark a DM "delivered" the
     * moment ANY node relayed it, which says nothing about whether the person it was
     * addressed to ever got it. A DM has a real acknowledgement from the destination
     * itself; it must wait for that one and only that one. */
    if (hdr.dest == MESH_ADDR_BROADCAST_ONAIR) {
      resolveAck(hdr.packetId, 0);
    }
    return false;
  }
  // Drop duplicate rebroadcasts of the same packet (mesh flooding).
  if (seenPacketId(hdr.packetId)) {
    /* ...but a REPEATED want-ack DM we already stored means our ACK was lost
     * and the sender is retrying: ACK again (stock does — ReliableRouter),
     * without re-storing. Only ids in recentAckIds — i.e. DMs we actually
     * accepted — are answered; anything else stays dropped. */
    if (hdr.dest == myNodeNum && (hdr.flags & MESH_FLAGS_WANT_ACK) && channelCount > 0) {
      for (int i = 0; i < 8; i++) {
        if (recentAckIds[i] == hdr.packetId && hdr.packetId != 0) {
          meshTxAck(myNodeNum, hdr.sender, hdr.packetId, &channels[0]);
          log_e("MESH DM: re-ACKed id=0x%08x for !%08x (first ACK likely lost)",
                (unsigned)hdr.packetId, (unsigned)hdr.sender);
          break;
        }
      }
    }
    return false;
  }

  // Every heard sender is a known node.
  MeshNode* n = upsertNode(hdr.sender, NULL);
  if (n) {
    n->snr = snr;
  }
  /* Direct earshot = the packet reached us with NO relay: hop_start (what the
   * sender set) still equals hop_limit (what is left). A node we only hear
   * through a repeater is NOT a neighbour, and claiming otherwise would draw
   * an edge on Nick's mesh map that does not exist. hop_start == 0 means the
   * sender never set it (pre-2.x): unknowable, so not counted. */
  {
    const uint8_t hopStart = (uint8_t)((hdr.flags & MESH_FLAGS_HOP_START_MASK)
                                       >> MESH_FLAGS_HOP_START_SHIFT);
    const uint8_t hopLimit = (uint8_t)(hdr.flags & MESH_FLAGS_HOP_LIMIT_MASK);
    neighborHeard(hdr.sender, snr, hopStart != 0 && hopStart == hopLimit);
  }

  // CLIENT role: relay (flood-route) packets not addressed only to us, if they
  // still have hops left. Rebroadcast the raw packet with hop_limit decremented
  // and relay_node set to us (works for channels we can't even decrypt).
  {
    uint8_t hopLimit = hdr.flags & MESH_FLAGS_HOP_LIMIT_MASK;
    bool toUsOnly = (hdr.dest != MESH_ADDR_BROADCAST_ONAIR && hdr.dest == myNodeNum);
    if (hopLimit > 0 && !toUsOnly) {
      uint8_t relay[MESH_PHY_MAX_PAYLOAD];
      memcpy(relay, buf, len);
      relay[12] = (uint8_t)((hdr.flags & ~MESH_FLAGS_HOP_LIMIT_MASK) |
                            ((hopLimit - 1) & MESH_FLAGS_HOP_LIMIT_MASK));   // flags
      relay[15] = (uint8_t)(myNodeNum & 0xFF);                              // relay_node
      scheduleRebroadcast(relay, len);
    }
  }

  // Map on-air broadcast (0xFFFFFFFF) to our internal channel address (0).
  uint32_t toInternal = (hdr.dest == MESH_ADDR_BROADCAST_ONAIR)
                        ? MESH_BROADCAST_ADDR : hdr.dest;

  // Find a configured channel matching this packet's channel hash.
  const MeshChannel* ch = findChannelByHash(hdr.channelHash);

  /* ── PKI DM? ─────────────────────────────────────────────────────────────────────
   * A PKC packet is a DM to us with channel-hash 0x00 (stock zeroes the byte on the
   * air — Router.cpp). Try the sender's session key FIRST: the CCM tag makes a wrong
   * guess fail loudly-to-us and invisibly-on-air, so on failure we simply fall
   * through to the channel path (hash 0x00 could in principle be a real channel).
   * This is how a 2.5+ node's DM reaches this phone at all — its firmware refuses
   * to send the legacy channel-encrypted form. */
  if (hdr.dest == myNodeNum && hdr.channelHash == 0x00 &&
      payloadLen > MESH_PKI_OVERHEAD && n && (n->pkiFlags & MESH_NODE_HAS_KEY)) {
    uint8_t skey[MESH_KEY_LEN];
    if (pkiKeyForNode(n, skey)) {           // superloop depth: derive allowed here
      uint8_t dec[MESH_PHY_MAX_PAYLOAD];
      size_t decLen = 0;
      if (meshPkiDecrypt(skey, hdr.sender, hdr.packetId,
                         buf + MESH_HEADER_LEN, payloadLen, dec, &decLen)) {
        const uint8_t* pl = NULL;
        size_t plLen = 0;
        bool wantResp = false;
        uint32_t reqId = 0;
        int portnum = meshParseData(dec, decLen, &pl, &plLen, &wantResp, &reqId);

        if (portnum == MESH_PORT_TEXT_MESSAGE && pl && plLen) {
          char text[MESH_TEXT_LEN];
          size_t nt = plLen < sizeof(text) - 1 ? plLen : sizeof(text) - 1;
          memcpy(text, pl, nt);
          text[nt] = '\0';

          /* ACK first — delivery is true regardless of what the text turns out
           * to be. Remember the id so a retransmission is re-ACKed, not re-stored. */
          if (hdr.flags & MESH_FLAGS_WANT_ACK) {
            recentAckIds[recentAckPos] = hdr.packetId;
            recentAckPos = (uint8_t)((recentAckPos + 1) % 8);
            if (channelCount > 0) {
              meshTxAck(myNodeNum, hdr.sender, hdr.packetId, &channels[0]);
            }
          }

          /* Same diversion policy as the channel path below, same reasons:
           * recognised-by-prefix service traffic is not a chat message. */
          if (bookSyncIsSyncText(text)) {
            bookSyncInboxPush(text, hdr.sender,
                              ntpClock.isTimeKnown() ? (uint32_t)ntpClock.getExactUtcTime() : 0);
            return false;
          }
          if (smsMirrorIsMirrorLine(text)) {
            int r = smsMirrorIngestLine(text);
            log_e("SMSMIRROR rx (PKI DM) from 0x%08X: %s", hdr.sender,
                  r > 0 ? "STORED" : (r == 0 ? "duplicate, ignored" : "REFUSED"));
            return false;
          }

          storeMessage(hdr.sender, toInternal, 0x00, text, false);
          /* log_e: the only compiled-in level, and the first PKI DM ever heard is
           * exactly the moment that must be visible on serial. */
          log_e("MESH PKI DM from !%08x id=0x%08x (%uB)%s", (unsigned)hdr.sender,
                (unsigned)hdr.packetId, (unsigned)plLen,
                (hdr.flags & MESH_FLAGS_WANT_ACK) ? " ACKed" : "");
          return true;                       // signal the UI to refresh

        } else if (portnum == MESH_PORT_ROUTING) {
          /* An ACK/NAK that was itself PKI-encrypted (stock normally sends these
           * on the channel — but be ready for either). error_reason is field 3
           * of the Routing payload. */
          uint32_t err = 0;
          if (pl && plLen >= 2 && pl[0] == 0x18) {
            err = pl[1];
          }
          log_e("MESH DM %s (PKI) from !%08x for id=0x%08x err=%u",
                err == 0 ? "ACK" : "NAK", (unsigned)hdr.sender, (unsigned)reqId, (unsigned)err);
          resolveAck(reqId, (uint8_t)err);   // stamp the receipt on the message it answers
          return false;
        } else if (portnum >= 0) {
          log_i("Mesh PKI pkt from 0x%08X port=%d (%uB)", hdr.sender, portnum, (unsigned)decLen);
          return false;
        }
      }
      // Auth failed: not PKI (or stale keys) — fall through to the channel path.
    }
  }

  if (payloadLen > 0 && ch) {
    // Known channel: decrypt with its key (AES-128 or AES-256) and decode.
    uint8_t dec[MESH_PHY_MAX_PAYLOAD];
    if (meshCryptCtr(ch->key, ch->keyLen, hdr.sender, hdr.packetId, buf + MESH_HEADER_LEN, payloadLen, dec)) {
      const uint8_t* pl = NULL;
      size_t plLen = 0;
      bool wantResp = false;
      uint32_t chReqId = 0;
      int portnum = meshParseData(dec, payloadLen, &pl, &plLen, &wantResp, &chReqId);

      /* (A per-packet log_e lived here on 2026-08-15 and answered its question: port 4
       * NodeInfo packets DO arrive and DO decrypt — `from=!33646708 port=4 payload=44B
       * wantResp=1 ch=0x08`. So names were never missing because NodeInfo was unheard; they
       * were missing because the node table was full and dropped every new node. Removed
       * again rather than left in: it fires on every packet, and log_e is the only level
       * compiled in, so it would bury the boot log forever.) */

      if (portnum == MESH_PORT_TEXT_MESSAGE && pl && plLen) {
        char text[MESH_TEXT_LEN];
        size_t nt = plLen < sizeof(text) - 1 ? plLen : sizeof(text) - 1;
        memcpy(text, pl, nt);
        text[nt] = '\0';

        /* Book-sync traffic is not a message. Park it for the reader and say nothing.
         *
         * ⚠ Recognised by PREFIX, deliberately before any attempt to check its signature —
         * exactly as COVEY does. This path has no business knowing the booksync passcode, and
         * a packet signed with somebody else's is still not a chat message: showing
         * "CBS1 MFRGG..." in the list would be worse than dropping it. Verification happens
         * in the reader, which is the only part that holds the key.
         *
         * Returning false rather than true keeps the new-message chime, vibration and unread
         * badge out of it: nothing arrived that a person needs to read. */
        if (bookSyncIsSyncText(text)) {
          bookSyncInboxPush(text, hdr.sender,
                            ntpClock.isTimeKnown() ? (uint32_t)ntpClock.getExactUtcTime() : 0);
          log_i("Mesh booksync packet from 0x%08X (%u parked)",
                hdr.sender, (unsigned)bookSyncInboxCount());
          return false;
        }

        /* A text message mirrored from COVEY. Same treatment as book-sync and for the same
         * reason: nobody sent this to you as a mesh message, so it has no business in the
         * Chats list. It goes straight into the SIP message store instead.
         *
         * ⚠ Recognised by PREFIX, before anything else. A packet that merely claims to be
         * one is dropped, not displayed — showing "CSM1 109970452 4257604281 i ..." as a
         * chat message would be worse than losing it.
         *
         * Returns FALSE, like book-sync: a mirrored record must not raise the MESH
         * new-message signal (see the long note at the bottom of this block). The SIP-side
         * announcing happens in the main loop via smsMirrorTakeNews(). */
        if (smsMirrorIsMirrorLine(text)) {
          int r = smsMirrorIngestLine(text);
          /* ⚠ log_e, NOT log_i, and on the SUCCESS path too. Only log_e is compiled into
           * this build, so an log_i here would make a working mirror and a broken one look
           * identical on serial — which is precisely the trap that made book sync look dead
           * for a whole session. This is the ONLY evidence that a mirrored text arrived:
           * nothing is drawn, nothing chimes, and the message lands in the SIP store rather
           * than anywhere the mesh app shows.
           * Dial it back to log_i once this path is trusted on hardware. */
          log_e("SMSMIRROR rx from 0x%08X: %s", hdr.sender,
                r > 0 ? "STORED" : (r == 0 ? "duplicate, ignored" : "REFUSED"));

          /* ⚠ THE ANNOUNCING MOVED — do not put a notify call back here.
           *
           * smsMirrorIngestLine() latches arrival news and the MAIN LOOP takes it once per
           * pass (smsMirrorTakeNews) and does the buzz + NEW_MESSAGE_EVENT — one announcer
           * for BOTH transports. That is the fix for "no vibrate on sip receive most of the
           * time": the LAN path stored texts with no notification at all, and because
           * ingest dedups by id, a LAN-first delivery permanently suppressed this path's
           * buzz too. Announcing from here as well would buzz an inbound LoRa arrival
           * twice. The buzz still uses the TEXT MESSAGES setting, not the Meshtastic one —
           * what arrived is a text; the radio is only how it got here. */

          /* ⚠ FALSE, like book-sync — and this reverses what it first did.
           *
           * Returning true raised the MESH new-message signal: chime, vibration and the
           * green mesh bubble. That was wrong, and using it made the mistake obvious. A
           * mirrored record is not news arriving on the mesh, it is a copy of something
           * that already happened somewhere else — usually a text NICK HIMSELF just sent
           * from COVEY. Chiming at someone to tell them they sent a message is noise.
           *
           * The right badge is still raised, just not from here: smsMirrorIngestLine()
           * refreshes `unreadMessages`, so a genuinely INCOMING mirrored text lights the
           * white SIP icon (the correct one — the message is in the SIP store, not the mesh
           * one), and an outgoing one lights nothing at all. */
          return false;
        }

        /* Replay-protocol traffic (RPL? / RPL / RPL.) — machine lines, never
         * chat. Without this filter every replayed record would land in the
         * Chats list of THIS phone, chiming (the unknown-prefix fallthrough is
         * chat — measured lesson from the booksync era). Only a REQUEST on the
         * sync channel acts; records/summaries we hear are someone else's
         * conversation with COVEY and are dropped silently. */
        if (replayIsReplayText(text)) {
          if (ch && strcasecmp(ch->name, "booksync") == 0) {
            replayHandleRequest(ch, text);
          }
          return false;
        }

        /* A DM that arrived channel-encrypted (a pre-2.5 sender — 2.5+ nodes
         * refuse to send this form). Accept it (unlike stock, which drops
         * "legacy DMs": our channels are private, the phishing argument doesn't
         * apply) and ACK it if asked, so the sender's UI shows delivery. */
        if (hdr.dest == myNodeNum && (hdr.flags & MESH_FLAGS_WANT_ACK)) {
          recentAckIds[recentAckPos] = hdr.packetId;
          recentAckPos = (uint8_t)((recentAckPos + 1) % 8);
          if (channelCount > 0) {
            meshTxAck(myNodeNum, hdr.sender, hdr.packetId, &channels[0]);
          }
        }

        /* ⚠ BROADCASTS ONLY — this dest check is the ONLY thing keeping DMs
         * out of the ring, and the spec's DM exclusion is worthless without it.
         * A legacy channel-encrypted DM (which this phone deliberately accepts,
         * just above) is decrypted with the CHANNEL key, so it is perfectly
         * readable here — and replaying one would BROADCAST a private message
         * to every booksync member. Only PKC DMs are undecryptable; the legacy
         * form breaks the assumption the spec leaned on. (Found by adversarial
         * review, 2026-08-21, before it ever ran in the woods.) */
        if (toInternal == MESH_BROADCAST_ADDR) {
          replayCapture(hdr.sender, ch, text);    // the ring remembers what COVEY may have missed
        }
        storeMessage(hdr.sender, toInternal, hdr.channelHash, text, false);   // real text!
        log_i("Mesh TEXT from 0x%08X on ch '%s': %s", hdr.sender, ch->name, text);
        return true;                       // signal the UI to refresh

      } else if (portnum == MESH_PORT_ROUTING && hdr.dest == myNodeNum) {
        /* ACK/NAK for a DM we sent — stock sends these channel-encrypted even
         * for PKI DMs (ROUTING is excluded from PKC). The only proof of
         * delivery this phone gets, so it goes to the compiled-in log level. */
        uint32_t err = 0;
        if (pl && plLen >= 2 && pl[0] == 0x18) {
          err = pl[1];
        }
        log_e("MESH DM %s from !%08x for id=0x%08x err=%u",
              err == 0 ? "ACK" : "NAK", (unsigned)hdr.sender, (unsigned)chReqId, (unsigned)err);
        resolveAck(chReqId, (uint8_t)err);   // stamp the receipt on the message it answers

      } else if (portnum == MESH_PORT_POSITION && pl && plLen) {
        // A node told the mesh where it is (COVEY does, every 5 minutes).
        int32_t latI, lonI;
        if (meshPosParse(pl, plLen, &latI, &lonI, NULL) && n) {
          n->latI = latI;
          n->lonI = lonI;
          n->posHeardMs = millis();
          dbDirty = true;
          placesNews = true;                 // refresh an open Nodes/Places view
          log_i("Mesh POSITION 0x%08X: %d,%d", hdr.sender, latI, lonI);
        }

      } else if (portnum == MESH_PORT_WAYPOINT && pl && plLen) {
        // A shared place (camp, the truck...) — the reference points for the UI.
        MeshWaypointMsg wp;
        if (meshWaypointParse(pl, plLen, &wp)) {
          /* Ownership first: a waypoint locked to a node may only be changed —
           * or deleted — by that node. Without this check, ANY holder of the
           * channel key (or a replayed packet) could silently delete or
           * relocate camp, the point every distance is measured from. */
          const MeshWaypoint* have = findWaypoint(wp.id);
          if (have && have->lockedTo != 0 && hdr.sender != have->lockedTo) {
            log_e("MESH WAYPOINT: refused change to locked '%s' from !%08x (owner !%08x)",
                  have->name, (unsigned)hdr.sender, (unsigned)have->lockedTo);
          }
          /* Deletions arrive two ways: a waypoint with NO position (the
           * convention — deleting on COVEY's map sends exactly this), or one
           * whose expire is already past (only honored when the clock is
           * known — dropping camp because NTP hasn't run yet would be worse
           * than showing a stale pin). */
          else if (!wp.hasPos ||
                   (wp.expire != 0 && ntpClock.isTimeKnown() &&
                    (uint32_t)ntpClock.getExactUtcTime() > wp.expire)) {   // UTC: expire is COVEY's
            for (int i = 0; i < waypointCount; i++) {
              if (waypoints[i].id == wp.id) {
                log_e("MESH WAYPOINT: '%s' removed%s", waypoints[i].name,
                      !wp.hasPos ? " (deleted by sender)" : " (expired on arrival)");
                waypoints[i] = waypoints[--waypointCount];
                dbDirty = true;
                placesNews = true;
                break;
              }
            }
          } else {
            upsertWaypoint(wp.id, wp.latI, wp.lonI, wp.expire, wp.lockedTo, wp.name);
          }
        }

      } else if (portnum == MESH_PORT_NODEINFO && pl && plLen) {
        // Learn the sender's friendly name — and their PKC public key (User
        // field 8), which is what unlocks DMs to them.
        char name[MESH_NAME_LEN];
        uint8_t peerKey[MESH_KEY_LEN];
        bool hasKey = false;
        MeshNode* infoNode = n;
        if (meshParseUserName(pl, plLen, name, sizeof(name), peerKey, &hasKey)) {
          infoNode = upsertNode(hdr.sender, name);
          log_i("Mesh NodeInfo 0x%08X: %s", hdr.sender, name);
        }
        if (hasKey && infoNode) {
          pkiLearnKey(infoNode, peerKey);   // TOFU; superloop depth (derive OK)
        }

        /* ── ANSWER WHEN SOMEBODY ASKS WHO WE ARE ────────────────────────────────────
         * want_response is how the rest of the mesh requests a NodeInfo: the Meshtastic
         * app's node-refresh, COVEY's "Ask info" and "Ask all to announce", and a stock
         * node meeting an unknown neighbour all set it. The phone never read the flag, so
         * it never replied, and on other radios it showed as a bare node number with no
         * name until it next announced on its own.
         *
         * ⚠ Reply with wantResponse=FALSE. Answering a request with another request is how
         * two nodes ping-pong forever and flood a shared channel.
         *
         * ⚠ Rate-limited. A single "ask everyone to announce" reaches every node in range
         * at once, and if they all answer immediately they collide and the airtime is
         * wasted. Real firmware damps these on purpose (COVEY measured one reply in 100 s
         * from a whole mesh). One reply per MESH_NODEINFO_REPLY_MIN_MS is plenty to be
         * discoverable without being a bad citizen. */
        if (wantResp) {
          const uint32_t now = millis();
          if (lastNodeInfoTxMs == 0 || (now - lastNodeInfoTxMs) > MESH_NODEINFO_REPLY_MIN_MS) {
            lastNodeInfoTxMs = now;
            announceNodeInfo(false);
            log_i("Mesh NodeInfo requested by 0x%08X - answered as '%s' (%s)",
                  hdr.sender, myLongName, myShortName);
          } else {
            log_i("Mesh NodeInfo requested by 0x%08X - damped (replied recently)", hdr.sender);
          }
        }

      } else {
        log_i("Mesh pkt from 0x%08X port=%d (%uB)", hdr.sender, portnum, payloadLen);
      }
    }
  } else {
    // Unknown channel (not configured): can't decrypt. Node is still tracked.
    log_i("Mesh enc pkt from 0x%08X ch=0x%02X (%uB, unknown channel)", hdr.sender, hdr.channelHash, payloadLen);
  }
#endif
  return false;
}

// ---- Storage helpers -------------------------------------------------------

void MeshtasticService::storeMessage(uint32_t from, uint32_t to, uint8_t channelHash,
                                     const char* text, bool outgoing) {
  if (!messages) {
    return;
  }
  // A "chat" is a channel (for broadcasts, keyed by channelHash) or a DM peer.
  bool bcast = (to == MESH_BROADCAST_ADDR);
  uint32_t peer = bcast ? 0 : (from == myNodeNum ? to : from);

  // Per-conversation cap: if this chat is full, drop its oldest message.
  int inChat = 0, oldestIdx = -1;
  for (int i = 0; i < msgCount; i++) {
    bool mb = (messages[i].to == MESH_BROADCAST_ADDR);
    bool same = bcast
                ? (mb && messages[i].channelHash == channelHash)
                : (!mb && (messages[i].from == myNodeNum ? messages[i].to : messages[i].from) == peer);
    if (same) {
      inChat++;
      if (oldestIdx < 0) oldestIdx = i;           // lowest index = oldest of this chat
    }
  }
  if (inChat >= MESH_MAX_PER_CHAT && oldestIdx >= 0) {
    removeMessageAt(oldestIdx);
  }

  // Global cap: drop the oldest message overall.
  if (msgCount >= MESH_MSG_CAP) {
    removeMessageAt(0);
  }

  MeshMessage& m = messages[msgCount++];          // append newest at the end
  m.from = from;
  m.to = to;
  m.channelHash = channelHash;
  m.timeMs = millis();
  // Our own sent messages are "read"; received ones start unread.
  m.flags = outgoing ? (MESH_MSG_OUTGOING | MESH_MSG_READ) : 0;
  strlcpy(m.text, text ? text : "", sizeof(m.text));
  lastStoredTimeMs = m.timeMs;    // notePendingAck() attaches the receipt to THIS message

  dbDirty = true;         // new message: persist soon
}

/* Remember that the message storeMessage() just appended is waiting on an ack for
 * `packetId`. Call it immediately after a successful send, while lastStoredTimeMs still
 * refers to the message that was just echoed locally. */
void MeshtasticService::notePendingAck(uint32_t packetId, uint32_t msgTimeMs) {
  if (!packetId || !msgTimeMs) {
    return;
  }
  PendingAck& slot = pendingAcks[pendingAckNext];
  pendingAckNext = (uint8_t)((pendingAckNext + 1) % MESH_ACK_PENDING);
  slot.packetId  = packetId;
  slot.msgTimeMs = msgTimeMs;
}

/* A routing packet came back naming one of ours. Mark that message delivered or failed.
 *
 * ⚠ A CHANNEL ACK IS NOT A DM ACK, and the UI must not present them as equal. For a DM the
 * acknowledgement comes from the destination node itself, so "delivered" means what it says.
 * For a broadcast, Meshtastic's ack is IMPLICIT — the first node to rebroadcast counts — so
 * it proves the message entered the mesh and nothing whatever about who received it. Both
 * set the same bit here; buildThread() words them differently, which is where the
 * distinction belongs. */
void MeshtasticService::resolveAck(uint32_t packetId, uint8_t errorReason) {
  if (!packetId || !messages) {
    return;
  }
  uint32_t when = 0;
  bool found = false;
  for (int i = 0; i < MESH_ACK_PENDING; i++) {
    if (pendingAcks[i].packetId == packetId) {
      when = pendingAcks[i].msgTimeMs;
      pendingAcks[i].packetId = 0;          // one ack per message; free the slot
      found = true;
      break;
    }
  }
  if (!found) {
    return;                                 // not ours, already resolved, or lost to a reboot
  }
  setMessageReceipt(when, errorReason == 0, errorReason);
}

/* Stamp one outgoing message's receipt, found by the timeMs that identifies it. Shared by
 * the ack path and by the queued-DM drain, which knows a send failed without any packet
 * ever reaching the air. */
/* Meshtastic's Routing.Error in words. The raw number is kept alongside because that is what
 * a forum post or a protobuf definition will match, but "err=6" on its own tells nobody in a
 * tree stand whether to move, wait, or re-send. Same table COVEY carries, same reason.
 * Only the codes this phone can actually provoke are spelled out; anything else prints raw. */
static const char* meshRoutingReason(uint8_t err) {
  switch (err) {
    case 0:  return "ok";
    case 1:  return "no route to them";
    case 2:  return "refused by the far end";
    case 3:  return "timed out";
    case 4:  return "no radio interface";
    case 5:  return "no reply after retries - out of range?";
    case 6:  return "they could not decrypt it - key mismatch (legacy DM to a 2.5+ node?)";
    case 7:  return "message too long";
    case 8:  return "no reply";
    case 9:  return "radio duty-cycle limit reached";
    case 34: return "encryption failed";
    case 35: return "their encryption key is unknown";
    default: return "unknown reason";
  }
}

void MeshtasticService::setMessageReceipt(uint32_t msgTimeMs, bool delivered, uint8_t err) {
  if (!messages || !msgTimeMs) {
    return;
  }
  for (int i = msgCount - 1; i >= 0; i--) {          // newest first: acks answer recent sends
    if (messages[i].timeMs == msgTimeMs && (messages[i].flags & MESH_MSG_OUTGOING)) {
      messages[i].flags &= (uint8_t)~(MESH_MSG_DELIVERED | MESH_MSG_FAILED);
      messages[i].flags |= delivered ? MESH_MSG_DELIVERED : MESH_MSG_FAILED;
      dbDirty = true;                       // the receipt persists with the message
      /* At the compiled-in level, because this is the only way to see a receipt land
       * without watching the screen — and a feature whose whole job is to report delivery
       * should be able to report that it reported it. Names the state the UI will show. */
      if (delivered) {
        log_e("MESH RECEIPT: '%s' -> %s", messages[i].text,
              (messages[i].to == MESH_BROADCAST_ADDR) ? "in mesh" : "delivered");
      } else {
        log_e("MESH RECEIPT: '%s' -> failed: %s (err=%u)", messages[i].text,
              meshRoutingReason(err), (unsigned)err);
      }
      return;
    }
  }
  log_e("MESH RECEIPT: ack for a message no longer in the store (t=%u)", (unsigned)msgTimeMs);
}

int MeshtasticService::getUnreadTotal() const {
  int u = 0;
  for (int i = 0; i < msgCount; i++) {
    if (!(messages[i].flags & MESH_MSG_READ)) u++;
  }
  return u;
}

void MeshtasticService::markRead(bool isChannel, uint8_t channelHash, uint32_t peer) {
  bool any = false;
  for (int i = 0; i < msgCount; i++) {
    MeshMessage& m = messages[i];
    if (m.flags & MESH_MSG_READ) {
      continue;
    }
    bool inChat;
    if (isChannel) {
      inChat = (m.to == MESH_BROADCAST_ADDR && m.channelHash == channelHash);
    } else {
      inChat = (m.to != MESH_BROADCAST_ADDR &&
                ((m.flags & MESH_MSG_OUTGOING) ? m.to : m.from) == peer);
    }
    if (inChat) {
      m.flags |= MESH_MSG_READ;
      any = true;
    }
  }
  if (any) {
    dbDirty = true;
  }
}

MeshNode* MeshtasticService::upsertNode(uint32_t nodeNum, const char* name) {
  if (nodeNum == 0) {
    return NULL;
  }
  // Existing?
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].nodeNum == nodeNum) {
      nodes[i].lastHeardMs = millis();
      orderDirty = true;               // "most recently heard" just changed
      if (name && name[0] && strcmp(nodes[i].name, name) != 0) {
        strlcpy(nodes[i].name, name, sizeof(nodes[i].name));
        dbDirty = true;   // name changed: persist soon
      }
      return &nodes[i];   // plain "heard" updates don't trigger a save
    }
  }
  /* ── TABLE FULL: EVICT THE LEAST-RECENTLY-HEARD, DO NOT DROP THE NEW ONE ──────────
   * This used to `return NULL` with a comment promising an eviction policy later, which
   * meant the node list froze permanently at the first 32 nodes ever heard and every node
   * discovered afterwards was silently discarded — including its NodeInfo, so names could
   * never be learned for anyone new.
   *
   * Measured 2026-08-15: the phone held exactly 32/32 nodes, all but its own stored under
   * a bare hex id, while the mesh around it had EIGHTY. The list was not just incomplete,
   * it was stuck.
   *
   * Evicting rather than growing the array is deliberate: `nodes[]` is a fixed member of a
   * long-lived object, so raising MESH_MAX_NODES costs internal RAM permanently, and
   * internal RAM is what panics this phone. A 32-entry most-recently-heard window is more
   * useful than a frozen 32-entry oldest-heard one, at identical cost.
   *
   * (Eviction order, and why a keyed peer outranks a stranger, is documented at the loop.) */
  if (nodeCount >= MESH_MAX_NODES) {
    /* 🔑 A NODE WHOSE PUBLIC KEY WE KNOW IS EVICTED LAST, and this is not a nicety — it is
     * the difference between DMs working and failing silently.
     *
     * MEASURED 2026-08-24: COVEY (!62b8d2fd) sat in this table as a bare '!62b8d2fd' with NO
     * KEY, while the docs record it as 'Nick H New Device' with its key learned months ago.
     * Oldest-heard eviction had thrown it out in favour of the 32nd stranger on LongFast —
     * a public channel full of nodes we will never DM — and the memset below (rightly) took
     * its public key with it. When COVEY next spoke it came back keyless, so every DM to it
     * fell back to the pre-2.5 legacy form, which COVEY refuses to decrypt. The delivery
     * receipt added the same day is what made that visible: `err=6, key mismatch`.
     *
     * A node we can do end-to-end crypto with is worth more than the 32nd stranger heard,
     * always. So: evict the oldest KEYLESS node; only if every one of them has a key does
     * this fall back to oldest-overall, which cannot deadlock and matches the old
     * behaviour exactly on a table full of keyed peers.
     *
     * ⚠ Never evict ourselves — we are in this table too (setup() upserts myNodeNum), and
     * losing our own entry would take our own name out of the UI.
     * ⚠ Signed difference for the comparison so it stays correct across millis() wraparound. */
    /* THREE TIERS, evicted worst-first, oldest-heard within a tier:
     *   0  a plain stranger        — the normal victim
     *   1  a peer whose key we know — costs working DMs to lose (see the note above)
     *   2  ⭐ starred by Nick       — he said this node matters; the mesh does not get a vote
     * A tier is only touched when every lower one is empty, so a table full of strangers can
     * never push out a starred node, which is most of the point of starring. */
    int victim[3] = { -1, -1, -1 };
    for (int i = 0; i < nodeCount; i++) {
      if (nodes[i].nodeNum == myNodeNum) {
        continue;
      }
      int tier = 0;
      if (nodes[i].pkiFlags & MESH_NODE_FAVOURITE) {
        tier = 2;
      } else if (nodes[i].pkiFlags & MESH_NODE_HAS_KEY) {
        tier = 1;
      }
      if (victim[tier] < 0 ||
          (int32_t)(nodes[i].lastHeardMs - nodes[victim[tier]].lastHeardMs) < 0) {
        victim[tier] = i;
      }
    }
    int oldest = victim[0] >= 0 ? victim[0] : (victim[1] >= 0 ? victim[1] : victim[2]);
    if (oldest < 0) {
      return NULL;                    // table is nothing but ourselves: nothing to evict
    }
    if (nodes[oldest].pkiFlags & (MESH_NODE_HAS_KEY | MESH_NODE_FAVOURITE)) {
      log_e("MESH NODE: evicting %s peer !%08x '%s' - no lesser node left to drop",
            (nodes[oldest].pkiFlags & MESH_NODE_FAVOURITE) ? "STARRED" : "KEYED",
            (unsigned)nodes[oldest].nodeNum, nodes[oldest].name);
    }
    orderDirty = true;
    MeshNode& ev = nodes[oldest];
    /* Wipe the WHOLE slot before reuse: without this the new node inherits the
     * evicted node's pubKey/pkiFlags (a key it never announced) and its
     * latI/lonI/posHeardMs (a position it never transmitted — which the UI
     * would then show as a fresh fix and saveDb would make permanent). */
    memset(&ev, 0, sizeof(ev));
    ev.nodeNum = nodeNum;
    ev.lastHeardMs = millis();
    applyFavouriteFlag(&ev);
    ev.snr = 0;
    if (name && name[0]) {
      strlcpy(ev.name, name, sizeof(ev.name));
    } else {
      snprintf(ev.name, sizeof(ev.name), "!%08x", nodeNum);
    }
    dbDirty = true;
    return &ev;
  }

  MeshNode& n = nodes[nodeCount];
  orderDirty = true;
  n.nodeNum = nodeNum;
  n.lastHeardMs = millis();
  applyFavouriteFlag(&n);          // a starred node that was evicted comes back starred
  n.snr = 0;
  if (name && name[0]) {
    strlcpy(n.name, name, sizeof(n.name));
  } else {
    snprintf(n.name, sizeof(n.name), "!%08x", nodeNum);
  }
  dbDirty = true;         // new node: persist soon
  return &nodes[nodeCount++];
}

// ---- Persistence (SPIFFS) --------------------------------------------------

#define MESH_DB_PATH      "/meshdb.bin"
#define MESH_DB_TMP       "/meshdb.tmp"
#define MESH_FAV_PATH     "/meshfav.bin"
#define MESH_FAV_TMP      "/meshfav.tmp"
#define MESH_FAV_MAGIC    0x31564146u   // "FAV1"
#define MESH_DB_MAGIC     0x314D5057u   // "WPM1"
/* v3: node position fields + waypoint tail. v4: MeshWaypoint gained lockedTo.
 *
 * ⚠ THE HEADER RECORDS EVERY STRUCT SIZE IT WRITES. v3 recorded the node and
 * message sizes but NOT the waypoint size, so when MeshWaypoint grew a field
 * the old tail was read with the new layout — misaligned garbage, and the
 * stored places vanished on the next boot (measured: 'vashon' disappeared,
 * 2026-08-19). Anything appended here needs its size in the header too. */
#define MESH_DB_VERSION   4

/* Older on-flash MeshNode layouts, byte-exact, so an existing database migrates
 * instead of being discarded (node names, keys and message history are worth
 * keeping — the struct-size header check would otherwise throw the file away). */
typedef struct {                        // v1: through 0.9.5
  uint32_t nodeNum;
  char     name[MESH_NAME_LEN];
  uint32_t lastHeardMs;
  int16_t  snr;
} MeshNodeV1;

typedef struct {                        // v2: 0.9.6 (PKC) — before positions
  uint32_t nodeNum;
  char     name[MESH_NAME_LEN];
  uint32_t lastHeardMs;
  int16_t  snr;
  uint8_t  pubKey[MESH_KEY_LEN];
  uint8_t  pkiFlags;
} MeshNodeV2;

/* ⚠ WRITE TO A TEMP FILE AND RENAME. THIS USED TO OPEN THE REAL FILE WITH "w", WHICH
 * TRUNCATES IT AND THEN REWRITES THE WHOLE DATABASE IN PLACE — so anything that stopped the
 * phone mid-write left a SHORT FILE, and loadDb() `break`s out of its read loops on a short
 * read and keeps whatever it managed to get. Silently. Nodes are written first so they
 * survive; messages come next and get truncated; waypoints are written LAST and go first.
 *
 * MEASURED 2026-08-24, and it is not theoretical: a night of flashing and serial resets (every
 * port open resets this board) took the message store from 55 down to 4, and the file still
 * loaded cleanly with 32 nodes — nothing anywhere reported a problem. A save runs on a dirty
 * flag, so the window is open far more often than "only during a manual save".
 *
 * The same pattern is already used by healthTrim() for exactly this reason; saveDb simply
 * never adopted it. Rename on SPIFFS is atomic, so a reset now leaves either the old complete
 * database or the new complete one, never a half of either. */
/* ── BUFFERED DB WRITER ────────────────────────────────────────────────────────────────────
 * saveDb() writes the database as ~55 separate File::write() calls — 4 bytes for the magic,
 * 84 per node, 248 per message. Every one of those crosses VFS into SPIFFS on its own, and
 * MEASURED on hardware that cost 1275 ms for 6,192 bytes: about five bytes per millisecond,
 * with the whole superloop — keypad, screen AND the WiFi stack — stopped for the duration.
 * That is Nick's "freeze for a second or two and WiFi drops" (2026-08-24), reproduced and
 * timed rather than guessed at.
 *
 * The size was never the problem; the CALL COUNT was. This batches into one PSRAM block and
 * hands SPIFFS a few large writes instead of dozens of small ones. PSRAM because internal RAM
 * is what panics this phone, and a save is not worth a kilobyte of it; if the allocation fails
 * it degrades to the old unbuffered behaviour rather than not saving. */
struct MeshDbWriter {
  File*    f;
  uint8_t* buf;
  size_t   cap, len;
  bool     ok;

  MeshDbWriter(File* file, size_t capacity) : f(file), buf(NULL), cap(capacity), len(0), ok(true) {
    buf = (uint8_t*)ps_malloc(capacity);
  }
  ~MeshDbWriter() {
    flush();
    if (buf) {
      free(buf);
    }
  }
  void flush() {
    if (buf && len) {
      ok = ok && (f->write(buf, len) == len);
      len = 0;
    }
  }
  void put(const void* p, size_t n) {
    if (!buf) {                          // no PSRAM: straight through, as before
      ok = ok && (f->write((const uint8_t*)p, n) == n);
      return;
    }
    if (n > cap) {                       // never fits: flush and pass it through whole
      flush();
      ok = ok && (f->write((const uint8_t*)p, n) == n);
      return;
    }
    if (len + n > cap) {
      flush();
    }
    memcpy(buf + len, p, n);
    len += n;
  }
};

/* ── WHICH FILESYSTEM THE MESH DATABASE LIVES ON ───────────────────────────────────────────
 * SD when a card is present, SPIFFS when it is not.
 *
 * BENCHMARKED ON THIS PHONE, 8 KB written the same shape a real save has (`bench` on serial):
 *
 *     SPIFFS   total 2599-2845 ms   [open 1645  write 163-809  rm 62-324  rename 326-639]
 *     SD       total    48-57 ms    [open   10  write      11  rm  15-24  rename      12]
 *
 * **About fifty times faster, and consistent** — no wear-levelling spikes across the passes,
 * which was the specific risk that made SD worth measuring rather than assuming.
 * ⚠ And note WHERE the SPIFFS time goes: `open` alone is 1.6 s. The write was never the
 * bottleneck. Creating a file is the pathological operation on this part, which the
 * temp-then-rename save hits on every single save.
 *
 * ⚠ SPIFFS REMAINS THE FALLBACK AND MUST KEEP WORKING. A card can be out, and `cardPresent`
 * was itself lying for the life of the project until 2026-08-24 — so this decides per call
 * rather than caching a verdict taken once at boot. loadDb() prefers whichever file exists,
 * so a database written to SPIFFS before this change is still found and is migrated to the
 * card by the next save. */
static fs::FS& meshFs() {
  return s_meshCardIn ? (fs::FS&)SD : (fs::FS&)SPIFFS;
}

/* 🛑 THE ONLY WRITER OF s_meshCardIn, AND IT HAS TO RUN BEFORE setup().
 *
 * meshFs() is consulted by loadDb() and loadFavourites(), both of which run inside setup().
 * Until 0.9.19 this flag was assigned only from loop(), so at load time it was still its
 * initial `false` and meshFs() answered SPIFFS — while every save after the first loop pass
 * wrote the SD card. MEASURED on both handsets 2026-08-26: /meshdb.bin on phone 1's card was
 * 9272 bytes and on phone 2's 6352 bytes (twenty-odd messages each), and both phones booted
 * showing an empty or years-stale chat list, because nothing ever opened those files. */
void MeshtasticService::setCardPresent(bool present) {
  cardIn = present;
  s_meshCardIn = present;
}

/* Which messages the next save writes. See mesh_retain.h for the rule and
 * MESH_PERSIST_* in the header for why the budget depends on the filesystem. */
int MeshtasticService::selectPersisted(uint8_t* keep) const {
  if (msgCount <= 0 || !messages) {
    return 0;
  }
  const int perChat = cardIn ? MESH_PERSIST_PER_CHAT_SD : MESH_PERSIST_PER_CHAT_FLASH;
  const int total   = cardIn ? MESH_PERSIST_TOTAL_SD    : MESH_PERSIST_TOTAL_FLASH;

  /* PSRAM, not the stack: 1000 messages is 8 KB of keys and the internal heap is the one
   * that panics this phone. If it cannot be had, say so and persist nothing rather than
   * silently writing a set nobody chose. */
  MeshRetainKey* keys = (MeshRetainKey*)ps_malloc((size_t)msgCount * sizeof(MeshRetainKey));
  if (!keys) {
    log_e("mesh: retention keys alloc failed (%d msgs)", msgCount);
    /* ⚠ CLEAR THE CALLER'S MASK BEFORE BAILING. Returning 0 with `keep` untouched left
     * saveDb() sizing its snapshot from 0 and then testing uninitialised ps_malloc bytes in
     * the write loop — copying a 248-byte message for every garbage non-zero byte, up to
     * ~248 KB written past the end of the buffer, into the PSRAM that holds the node table,
     * the book text and every widget. Silent, unbounded, and it would be blamed on anything
     * but this. Done here rather than in saveDb() to honour this function's own contract. */
    memset(keep, 0, (size_t)msgCount);
    return 0;
  }
  for (int i = 0; i < msgCount; i++) {
    const bool bcast = (messages[i].to == MESH_BROADCAST_ADDR);
    keys[i].isChannel = bcast;
    keys[i].id = bcast ? (uint32_t)messages[i].channelHash
                       : (messages[i].from == myNodeNum ? messages[i].to : messages[i].from);
  }

  int kept;
  if (keep) {
    kept = meshRetainSelect(keys, msgCount, perChat, total, MESH_RETAIN_MAX_CHATS, keep);
  } else {
    uint8_t* tmp = (uint8_t*)ps_malloc((size_t)msgCount);
    if (!tmp) {
      free(keys);
      return 0;
    }
    kept = meshRetainSelect(keys, msgCount, perChat, total, MESH_RETAIN_MAX_CHATS, tmp);
    free(tmp);
  }
  free(keys);
  return kept;
}

int MeshtasticService::persistedMessageCount() const {
  return selectPersisted(NULL);
}

/* The temp file stays open across loop passes while a save drains. A file-static rather than a
 * member so meshtastic_service.h does not have to include FS.h for every one of its consumers. */
static File s_saveFile;

/* ⚠ CHUNKING WAS TRIED AT 128 BYTES AND IT MADE THINGS WORSE. MEASURED, not theorised: three
 * saves produced **31** stalls over 250 ms, against a handful before. The per-byte model that
 * motivated it (~6 KB/s, so 128 B ≈ 20 ms) is an AVERAGE and the average is a lie here —
 * SPIFFS writes are quick until one crosses a block boundary and forces an ERASE, and an erase
 * is atomic and blocking however small the write that triggered it. More, smaller writes means
 * more boundary crossings, so more multi-hundred-ms pauses, not fewer.
 *
 * **A stall the filesystem takes in one indivisible piece cannot be chunked around.** So the
 * image goes out in a single write, which is at least ONE pause instead of many, and the
 * `uiIdle` gate on starting a save is what keeps that pause off the user's finger. The step
 * machinery is kept because it costs nothing and is exactly the right shape for the real fix.
 *
 * ⚠ THE REAL FIX IS TO STOP USING SPIFFS FOR THIS. See the changelog: /health.log already lives
 * on the SD card and appends every minute without ever tripping the stall detector. Moving the
 * database there is a deliberate piece of work — it needs a SPIFFS fallback for a missing card
 * — and it is the thing to do next, not another attempt at outsmarting this filesystem. */
#define MESH_SAVE_CHUNK_BYTES  65536

void MeshtasticService::saveDb() {
  /* Builds the whole image in PSRAM — memcpy only, NO file I/O — and hands it to saveDbStep()
   * to dribble out. See the note in the header for why the write is spread out at all. */
  if (saveActive) {
    dbDirty = true;          // a save is already draining; re-snapshot after it finishes
    return;
  }
  const uint32_t saveT0 = millis();

  /* Which messages are worth writing: the newest MESH_PERSIST_PER_CHAT_* of each
   * conversation, under MESH_PERSIST_TOTAL_*. The ring in RAM keeps more (MESH_MAX_PER_CHAT);
   * what a reboot gives back is this. */
  uint8_t* keep = NULL;
  int keptCount = 0;
  if (msgCount > 0) {
    keep = (uint8_t*)ps_malloc((size_t)msgCount);
    if (keep) {
      keptCount = selectPersisted(keep);
    } else {
      /* No PSRAM for the mask. Degrade to writing everything rather than to writing
       * nothing — a slow save beats a lost history. */
      log_e("mesh: retention mask alloc failed - saving the whole ring (%d msgs)", msgCount);
      keptCount = msgCount;
    }
  }

  const uint32_t hdrLen = 4 + 2 + 2 + 2 + 2;
  const uint32_t need = hdrLen
                        + 4 + (uint32_t)nodeCount * sizeof(MeshNode)
                        + 4 + (uint32_t)keptCount * sizeof(MeshMessage)
                        + 4 + (uint32_t)waypointCount * sizeof(MeshWaypoint);
  uint8_t* buf = (uint8_t*)ps_malloc(need);
  if (!buf) {
    log_e("mesh: saveDb PSRAM snapshot failed (%u bytes) - not saving", (unsigned)need);
    if (keep) {
      free(keep);
    }
    return;
  }

  uint32_t o = 0;
  #define PUT(p, n)  do { memcpy(buf + o, (p), (n)); o += (n); } while (0)
  uint32_t magic = MESH_DB_MAGIC;
  uint16_t ver = MESH_DB_VERSION;
  uint16_t ns = (uint16_t)sizeof(MeshNode);
  uint16_t ms = (uint16_t)sizeof(MeshMessage);
  uint16_t ws = (uint16_t)sizeof(MeshWaypoint);
  PUT(&magic, 4);
  PUT(&ver, 2);
  PUT(&ns, 2);
  PUT(&ms, 2);
  PUT(&ws, 2);
  int32_t nc = nodeCount;
  PUT(&nc, sizeof(nc));
  for (int i = 0; i < nodeCount; i++) {
    PUT(&nodes[i], sizeof(MeshNode));
  }
  int32_t mc = keptCount;
  PUT(&mc, sizeof(mc));
  /* `messages` is already oldest-at-[0], so a forward walk writes them oldest -> newest,
   * which is the order loadDb() reads. (This used to index through getMessage(), which is
   * newest-first, and counted backwards to compensate — same bytes, one less thing to get
   * backwards while adding a filter to it.) */
  for (int i = 0; i < msgCount; i++) {
    if (!keep || keep[i]) {
      PUT(&messages[i], sizeof(MeshMessage));
    }
  }
  int32_t wc = waypointCount;
  PUT(&wc, sizeof(wc));
  for (int i = 0; i < waypointCount; i++) {
    PUT(&waypoints[i], sizeof(MeshWaypoint));
  }
  #undef PUT

  /* No remove() first: "w" truncates an existing file or creates a new one, so the remove was
   * a redundant SPIFFS metadata operation — and on this part those are expensive. It also
   * logged `/meshdb.tmp does not exists` on every single save, which is the normal case and
   * therefore pure noise in a log people read to find real faults. */
  if (keep) {
    free(keep);
  }

  s_saveFile = meshFs().open(MESH_DB_TMP, "w");
  if (!s_saveFile) {
    log_e("mesh: saveDb open failed");
    free(buf);
    return;
  }
  saveBuf = buf;
  saveLen = o;
  saveOff = 0;
  saveActive = true;
  log_i("mesh: save started -> %s, %u bytes (%d of %d msgs kept) snapshotted in %u ms",
        cardIn ? "SD" : "SPIFFS", (unsigned)saveLen, keptCount, msgCount,
        (unsigned)(millis() - saveT0));
}

void MeshtasticService::saveDbStep() {
  if (!saveActive) {
    return;
  }
  const uint32_t n = (saveLen - saveOff) < MESH_SAVE_CHUNK_BYTES
                     ? (saveLen - saveOff) : MESH_SAVE_CHUNK_BYTES;
  if (n) {
    if (s_saveFile.write(saveBuf + saveOff, n) != n) {
      log_e("mesh: saveDb write FAILED at %u/%u - database left as it was",
            (unsigned)saveOff, (unsigned)saveLen);
      s_saveFile.close();
      meshFs().remove(MESH_DB_TMP);
      free(saveBuf);
      saveBuf = NULL;
      saveActive = false;
      return;
    }
    saveOff += n;
  }
  if (saveOff < saveLen) {
    return;                                  // more next pass
  }
  s_saveFile.close();
  free(saveBuf);
  saveBuf = NULL;
  saveActive = false;
  /* Only now does the real file change: an interrupted save leaves the OLD database intact. */
  meshFs().remove(MESH_DB_PATH);
  if (!meshFs().rename(MESH_DB_TMP, MESH_DB_PATH)) {
    log_e("mesh: saveDb RENAME FAILED - database left as it was");
    return;
  }
  log_i("mesh: saved %d nodes, %d msgs, %d waypoints", nodeCount, msgCount, waypointCount);
}

void MeshtasticService::loadDb() {
  orderDirty = true;                       // freshly loaded: nothing is sorted yet
  /* Prefer the card, then fall back to a database written before the move — that one is
   * migrated to the card by the next save.
   *
   * ⚠ SPELLED OUT RATHER THAN meshFs(), DELIBERATELY. Both branches used to resolve to
   * SPIFFS here, because the flag meshFs() reads was not set until the first loop pass, and
   * this runs in setup(). The fallback looked like a fallback and was actually the same file
   * twice, so the card's database was unreachable at boot no matter what was in it.
   * setCardPresent() is now called before setup() as well, and naming the two filesystems
   * here means a future reordering cannot re-create that silently. */
  File f;
  dbSource = "none";
  if (cardIn) {
    f = SD.open(MESH_DB_PATH, "r");
    if (f) {
      dbSource = "SD";
    }
  }
  if (!f) {
    f = SPIFFS.open(MESH_DB_PATH, "r");
    if (f) {
      dbSource = "SPIFFS";
    }
  }
  if (!f) {
    return;                             // first boot: nothing saved yet
  }
  uint32_t magic = 0;
  uint16_t ver = 0, ns = 0, ms = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != MESH_DB_MAGIC) {
    log_e("MESH DB: %s/%s has a bad magic - ignored", dbSource, MESH_DB_PATH);
    dbSource = "none";
    f.close();
    return;
  }
  f.read((uint8_t*)&ver, 2);
  f.read((uint8_t*)&ns, 2);
  f.read((uint8_t*)&ms, 2);
  uint16_t ws = 0;
  if (ver >= 4) {                           // v1..v3 have no waypoint-size field
    f.read((uint8_t*)&ws, 2);
  }
  // Struct-layout guard: ignore stale data written by a different build —
  // EXCEPT the known older layouts, which are migrated in place.
  const bool v1 = (ver == 1 && ns == sizeof(MeshNodeV1) && ms == sizeof(MeshMessage));
  const bool v2 = (ver == 2 && ns == sizeof(MeshNodeV2) && ms == sizeof(MeshMessage));
  /* v3 has today's node layout but an older, SHORTER waypoint record and no
   * waypoint-size field. Nodes and messages migrate; its waypoint tail cannot
   * be read with the current struct, so it is skipped and the places re-learn
   * from the mesh (COVEY re-shares a pin when it is edited). */
  const bool v3 = (ver == 3 && ns == sizeof(MeshNode) && ms == sizeof(MeshMessage));
  if (!v1 && !v2 && !v3 &&
      (ver != MESH_DB_VERSION || ns != sizeof(MeshNode) || ms != sizeof(MeshMessage) ||
       ws != sizeof(MeshWaypoint))) {
    /* A layout this build cannot read. Saying so matters: this is the one path that throws
     * away a person's whole history on purpose, and it used to do it without a word — so a
     * struct change and a filesystem mix-up presented identically (an empty chat list). */
    log_e("MESH DB: %s copy is v%d (node %d msg %d wp %d) and this build wants v%d "
          "(%d/%d/%d) - history NOT loaded",
          dbSource, (int)ver, (int)ns, (int)ms, (int)ws, MESH_DB_VERSION,
          (int)sizeof(MeshNode), (int)sizeof(MeshMessage), (int)sizeof(MeshWaypoint));
    dbSource = "none";
    f.close();
    return;
  }

  int32_t nc = 0;
  if (f.read((uint8_t*)&nc, sizeof(nc)) != sizeof(nc)) { f.close(); return; }
  if (nc < 0) nc = 0;
  if (nc > MESH_MAX_NODES) nc = MESH_MAX_NODES;
  nodeCount = 0;
  for (int i = 0; i < nc; i++) {
    MeshNode n;
    memset(&n, 0, sizeof(n));              // migrated fields start empty
    if (v1) {
      MeshNodeV1 o;
      if (f.read((uint8_t*)&o, sizeof(o)) != sizeof(o)) break;
      n.nodeNum     = o.nodeNum;
      memcpy(n.name, o.name, sizeof(n.name));
      n.lastHeardMs = o.lastHeardMs;
      n.snr         = o.snr;
    } else if (v2) {
      MeshNodeV2 o;
      if (f.read((uint8_t*)&o, sizeof(o)) != sizeof(o)) break;
      n.nodeNum     = o.nodeNum;
      memcpy(n.name, o.name, sizeof(n.name));
      n.lastHeardMs = o.lastHeardMs;
      n.snr         = o.snr;
      memcpy(n.pubKey, o.pubKey, sizeof(n.pubKey));   // PKC keys SURVIVE
      n.pkiFlags    = o.pkiFlags;
    } else {
      if (f.read((uint8_t*)&n, sizeof(n)) != sizeof(n)) break;
    }
    applyFavouriteFlag(&n);              // the star list outranks whatever the DB held
    nodes[nodeCount++] = n;
  }
  if (v1 || v2 || v3) {
    dbDirty = true;                        // rewritten in the current layout on the next debounce
    log_e("MESH DB: migrated %d node(s) from v%d%s", nodeCount, v1 ? 1 : (v2 ? 2 : 3),
          v3 ? " (places re-learn from the mesh)" : "");
  }

  /* ⚠ The message COUNT is read UNCONDITIONALLY. Guarding the read behind
   * `messages &&` (as this once did) would leave the file cursor at the count
   * when the buffer alloc failed — and the waypoint tail below would then parse
   * message bytes as waypoints and re-persist the garbage as real. */
  int32_t mc = 0;
  if (f.read((uint8_t*)&mc, sizeof(mc)) == sizeof(mc)) {
    if (mc < 0) mc = 0;
    if (messages) {
      int32_t take = mc > MESH_MSG_CAP ? MESH_MSG_CAP : mc;
      msgCount = 0;
      for (int i = 0; i < take; i++) {   // stored oldest -> newest
        MeshMessage m;
        if (f.read((uint8_t*)&m, sizeof(m)) != sizeof(m)) break;
        messages[msgCount++] = m;
      }
      // Skip any records beyond the cap so the tail stays aligned.
      if (mc > take && msgCount == take) {
        f.seek(f.position() + (uint32_t)(mc - take) * sizeof(MeshMessage));
      }
    } else {
      f.seek(f.position() + (uint32_t)mc * sizeof(MeshMessage));
    }
  }

  // v4 tail: waypoints. Skipped for every migrated layout (v1/v2 have no tail,
  // v3's uses a shorter record) — those places re-learn from the mesh.
  if (!v1 && !v2 && !v3) {
    int32_t wc = 0;
    if (f.read((uint8_t*)&wc, sizeof(wc)) == sizeof(wc)) {
      if (wc < 0) wc = 0;
      if (wc > MESH_MAX_WAYPOINTS) wc = MESH_MAX_WAYPOINTS;
      waypointCount = 0;
      for (int i = 0; i < wc; i++) {
        MeshWaypoint w;
        if (f.read((uint8_t*)&w, sizeof(w)) != sizeof(w)) break;
        if (w.id != 0) {
          w.name[sizeof(w.name) - 1] = '\0';   // never trust flash to terminate
          w.heardMs = 0;                       // a previous boot's millis() is meaningless
          waypoints[waypointCount++] = w;      // (0 = restored: evict these first)
        }
      }
    }
  }

  /* Restored node positions carry a previous boot's millis(). Rebase to the
   * 1-tick sentinel: "position known, age unknown" — the UI says 'old' instead
   * of computing a 71,000-minute lie. */
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].posHeardMs != 0) {
      nodes[i].posHeardMs = 1;
    }
  }

  f.close();
  /* log_e, not log_i, and the filesystem is named. log_i does not survive this build's log
   * level, so the one line that would have shown the phone reading the wrong file was
   * invisible on the very cable people were watching. `meshdb` on the console prints the
   * same facts on demand. */
  log_e("MESH DB: loaded %d nodes, %d msgs, %d places from %s", nodeCount, msgCount,
        waypointCount, dbSource);
}

void MeshtasticService::seedStubData() {
  // One demo peer and a welcome message so the UI shows structure. Removed
  // once the real radio provides live nodes/messages.
  upsertNode(0xC0FFEE01u, "Base");
  storeMessage(0xC0FFEE01u, MESH_BROADCAST_ADDR, channelCount ? channels[0].hash : 0,
               "Welcome to Meshtastic (stub). Radio PHY coming soon.", false);
}

// ---- UI-facing API ---------------------------------------------------------

int MeshtasticService::getMessageCount() const {
  return msgCount;
}

const MeshMessage* MeshtasticService::getMessage(int index) const {
  // Newest-first: index 0 == most recently stored (array is oldest-first).
  if (!messages || index < 0 || index >= msgCount) {
    return NULL;
  }
  return &messages[msgCount - 1 - index];
}

int MeshtasticService::getNodeCount() const {
  return nodeCount;
}

/* Starred first, then most-recently-heard. Rebuilt lazily — the list is read far less often
 * than it is written (every heard packet stamps lastHeardMs), so sorting on read is much the
 * cheaper end. Insertion sort: nearly-sorted input is the normal case and 200 entries of a
 * uint16 index is nothing. */
void MeshtasticService::rebuildNodeOrder() {
  if (!nodes || !nodeOrder) {
    return;
  }
  for (int i = 0; i < nodeCount; i++) {
    nodeOrder[i] = (uint16_t)i;
  }
  for (int i = 1; i < nodeCount; i++) {
    uint16_t v = nodeOrder[i];
    int j = i - 1;
    while (j >= 0) {
      const MeshNode& a = nodes[nodeOrder[j]];
      const MeshNode& b = nodes[v];
      const bool aFav = (a.pkiFlags & MESH_NODE_FAVOURITE) != 0;
      const bool bFav = (b.pkiFlags & MESH_NODE_FAVOURITE) != 0;
      bool bBeforeA;
      if (aFav != bFav) {
        bBeforeA = bFav;                                   // starred outranks everything
      } else {
        bBeforeA = (int32_t)(b.lastHeardMs - a.lastHeardMs) > 0;   // wrap-safe: newer first
      }
      if (!bBeforeA) {
        break;
      }
      nodeOrder[j + 1] = nodeOrder[j];
      j--;
    }
    nodeOrder[j + 1] = v;
  }
  orderDirty = false;
  orderCount = nodeCount;
}

void MeshtasticService::refreshNodeOrder() {
  rebuildNodeOrder();
}

const MeshNode* MeshtasticService::getNode(int index) const {
  if (index < 0 || index >= nodeCount || !nodes) {
    return NULL;
  }
  if (!nodeOrder) {
    return &nodes[index];                                  // alloc failed: unsorted beats none
  }
  /* ⚠ A DIRTY ORDER IS NOT RE-SORTED HERE, AND THAT IS DELIBERATE. `index` is a row number
   * the UI got from a list it drew earlier, and the Nodes screen uses it for hit-testing as
   * well as drawing. Re-sorting on read means a node heard between the draw and the key press
   * silently renumbers the rows, so `*` stars — or OK opens a DM to — the node NEXT to the
   * highlighted one. (COVEY had the same bug in its _activate_at and it was fixed the same
   * day.) A list that reshuffles under a finger is worse than a slightly stale one.
   * Only a size change forces a rebuild, because then the indices genuinely are invalid.
   * The UI re-sorts explicitly via refreshNodeOrder() when it rebuilds the screen. */
  if (orderCount != nodeCount) {
    const_cast<MeshtasticService*>(this)->rebuildNodeOrder();
  }
  return &nodes[nodeOrder[index]];
}

void MeshtasticService::applyFavouriteFlag(MeshNode* n) const {
  if (!n) {
    return;
  }
  for (int i = 0; i < favCount; i++) {
    if (favIds[i] == n->nodeNum) {
      n->pkiFlags |= MESH_NODE_FAVOURITE;
      return;
    }
  }
  n->pkiFlags &= (uint8_t)~MESH_NODE_FAVOURITE;
}

void MeshtasticService::loadFavourites() {
  favCount = 0;
  /* Same two filesystems, named for the same reason as loadDb() — this also runs in setup()
   * and also read SPIFFS twice while every star the user set was written to the card. */
  File f;
  if (cardIn) {
    f = SD.open(MESH_FAV_PATH, "r");
  }
  if (!f) {
    f = SPIFFS.open(MESH_FAV_PATH, "r");     // starred list from before the move
  }
  if (!f) {
    return;                                  // never starred anything yet
  }
  uint32_t magic = 0;
  uint8_t n = 0;
  if (f.read((uint8_t*)&magic, 4) == 4 && magic == MESH_FAV_MAGIC &&
      f.read(&n, 1) == 1) {
    if (n > MESH_MAX_FAVOURITES) {
      n = MESH_MAX_FAVOURITES;
    }
    for (uint8_t i = 0; i < n; i++) {
      uint32_t id = 0;
      if (f.read((uint8_t*)&id, 4) != 4) {
        break;                               // short file: keep what we got
      }
      if (id) {
        favIds[favCount++] = id;
      }
    }
  }
  f.close();
  if (favCount) {
    log_e("MESH: %d starred node(s) restored", (int)favCount);
  }
}

/* Temp-then-rename, for the same reason saveDb now does it: this file is small enough to be
 * written in one go, but a reset landing mid-write would otherwise cost the whole list — and
 * unlike the node table, nothing can rebuild it. */
void MeshtasticService::saveFavourites() {
  meshFs().remove(MESH_FAV_TMP);
  File f = meshFs().open(MESH_FAV_TMP, "w");
  if (!f) {
    log_e("mesh: saveFavourites open failed");
    return;
  }
  uint32_t magic = MESH_FAV_MAGIC;
  f.write((const uint8_t*)&magic, 4);
  f.write((const uint8_t*)&favCount, 1);
  for (int i = 0; i < favCount; i++) {
    f.write((const uint8_t*)&favIds[i], 4);
  }
  f.close();
  meshFs().remove(MESH_FAV_PATH);
  if (!meshFs().rename(MESH_FAV_TMP, MESH_FAV_PATH)) {
    log_e("mesh: saveFavourites RENAME FAILED - star list left as it was");
  }
}

bool MeshtasticService::isFavourite(uint32_t nodeNum) const {
  const MeshNode* n = findNode(nodeNum);
  return n && (n->pkiFlags & MESH_NODE_FAVOURITE);
}

bool MeshtasticService::toggleFavourite(uint32_t nodeNum) {
  int at = -1;
  for (int i = 0; i < favCount; i++) {
    if (favIds[i] == nodeNum) {
      at = i;
      break;
    }
  }
  bool on;
  if (at >= 0) {
    for (int i = at; i < favCount - 1; i++) {
      favIds[i] = favIds[i + 1];
    }
    favCount--;
    on = false;
  } else {
    if (favCount >= MESH_MAX_FAVOURITES) {
      log_e("MESH NODE: star list is full (%d) - unstar something first", MESH_MAX_FAVOURITES);
      return false;
    }
    favIds[favCount++] = nodeNum;
    on = true;
  }
  /* The list is the truth; the flag on the node is a cache of it. */
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].nodeNum == nodeNum) {
      applyFavouriteFlag(&nodes[i]);
      log_e("MESH NODE: !%08x '%s' %s", (unsigned)nodeNum, nodes[i].name,
            on ? "STARRED" : "unstarred");
      break;
    }
  }
  orderDirty = true;
  /* ⚠ DO NOT WRITE SPIFFS HERE. This is reached from the key handler on the Nodes screen, and
   * a filesystem write on the input path is the same shape of mistake that got the 80 MHz
   * experiment backed out — that one called setCpuFrequencyMhz() from inside the key-drain
   * loop. Nick starred a node on 2026-08-24 and the phone panicked (health.log:
   * `BOOT reset_reason=4`) seconds later; the star itself had already been written, which is
   * what points at the write rather than at the bookkeeping above it.
   * The house pattern is a dirty flag drained by loop() — the same thing dbDirty does — so
   * the star is durable within a tick without the input path ever touching flash. */
  favDirty = true;
  return on;
}

const MeshNode* MeshtasticService::findNode(uint32_t nodeNum) const {
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].nodeNum == nodeNum) {
      return &nodes[i];
    }
  }
  return NULL;
}

// ---- Channels --------------------------------------------------------------

int MeshtasticService::getChannelCount() const {
  return channelCount;
}

const MeshChannel* MeshtasticService::getChannel(int index) const {
  if (index < 0 || index >= channelCount) {
    return NULL;
  }
  return &channels[index];
}

const MeshChannel* MeshtasticService::findChannelByHash(uint8_t hash) const {
  for (int i = 0; i < channelCount; i++) {
    if (channels[i].hash == hash) {
      return &channels[i];
    }
  }
  return NULL;
}

void MeshtasticService::initDefaultChannel() {
  MeshChannel& c = channels[0];
  memset(&c, 0, sizeof(c));
  strlcpy(c.name, "LongFast", sizeof(c.name));
  memcpy(c.key, meshDefaultKey(), 16);
  c.keyLen = 16;
  c.hash = meshChannelHash(c.name, c.key, c.keyLen);
  channelCount = 1;
}

bool MeshtasticService::addChannel(const char* name, const uint8_t* key, uint8_t keyLen) {
  if (!name || !name[0] || (keyLen != 16 && keyLen != 32)) {
    return false;
  }
  // Merge: update an existing channel with the same name.
  for (int i = 0; i < channelCount; i++) {
    if (strcmp(channels[i].name, name) == 0) {
      memcpy(channels[i].key, key, keyLen);
      channels[i].keyLen = keyLen;
      channels[i].hash = meshChannelHash(name, key, keyLen);
      return true;
    }
  }
  if (channelCount >= MESH_MAX_CHANNELS) {
    return false;
  }
  MeshChannel& c = channels[channelCount];
  memset(&c, 0, sizeof(c));
  strlcpy(c.name, name, sizeof(c.name));
  memcpy(c.key, key, keyLen);
  c.keyLen = keyLen;
  c.hash = meshChannelHash(name, key, keyLen);
  channelCount++;
  return true;
}

// Base64 value of a char (accepts std and url alphabets); -1 for non-base64.
static int meshB64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

static int meshB64Decode(const char* in, uint8_t* out, int outCap) {
  int acc = 0, bits = 0, n = 0;
  for (const char* p = in; *p; p++) {
    if (*p == '=') break;
    int v = meshB64Val(*p);
    if (v < 0) continue;                 // skip whitespace / other chars
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n < outCap) out[n++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return n;
}

// Parse a varint at data[*i], advance *i. Returns the value.
static uint32_t meshPbVarint(const uint8_t* data, size_t len, size_t* i) {
  uint32_t v = 0; int sh = 0;
  while (*i < len && (data[*i] & 0x80)) { v |= (uint32_t)(data[*i] & 0x7f) << sh; sh += 7; (*i)++; }
  if (*i < len) { v |= (uint32_t)(data[*i] & 0x7f) << sh; (*i)++; }
  return v;
}

int MeshtasticService::applyChannelUrl(const char* url) {
  if (!url) {
    return 0;
  }
  // Flexible: take everything after the last '#' if present, else the whole
  // string; trim leading whitespace. Base64 decode handles -_ or +/ and skips
  // stray whitespace/newlines.
  const char* frag = strrchr(url, '#');
  frag = frag ? frag + 1 : url;
  while (*frag == ' ' || *frag == '\t' || *frag == '\n' || *frag == '\r') frag++;

  uint8_t data[256];
  int len = meshB64Decode(frag, data, sizeof(data));
  if (len < 2) {
    return 0;
  }

  // ChannelSet: repeated field 1 = ChannelSettings { field 2 psk, field 3 name }.
  int added = 0;
  size_t i = 0;
  while (i < (size_t)len) {
    uint8_t tag = data[i++];
    int field = tag >> 3, wire = tag & 7;
    if (wire == 2) {
      uint32_t l = meshPbVarint(data, len, &i);
      const uint8_t* sub = data + i;
      /* 🛑 CLAMP BY SUBTRACTION, NEVER BY ADDITION. This was `(i + l <= (size_t)len) ? l : ...`
       * and `i + l` is 32-bit arithmetic that WRAPS: a length varint of 0xFFFFFFFE (the bytes
       * FE FF FF FF 0F) with i == 8 makes `i + l` == 6, which passes `<= len`, and sublen
       * becomes 0xFFFFFFFE — then the loop below walks off a 256-byte STACK buffer.
       * ⚠ Reachable from a message received over the air: app_meshtastic's "Apply link" hands
       * a message's own text straight to this function.
       * Every other protobuf walker in this repo already clamps the safe way — mesh_pos.cpp:63
       * is `if (l > len - i) return false;` — and this one receive-side walker missed it. */
      size_t sublen = (l > (size_t)len - i) ? ((size_t)len - i) : (size_t)l;
      if (field == 1) {
        const uint8_t* psk = NULL; size_t pskLen = 0;
        char cname[MESH_NAME_LEN] = {0};
        size_t j = 0;
        while (j < sublen) {
          uint8_t st = sub[j++]; int sf = st >> 3, sw = st & 7;
          if (sw == 2) {
            size_t jj = j; uint32_t sl = meshPbVarint(sub, sublen, &jj); j = jj;
            // Same wrap, same clamp — see the note above.
            const uint8_t* sv = sub + j; size_t svlen = (sl > sublen - j) ? (sublen - j) : (size_t)sl;
            if (sf == 2) { psk = sv; pskLen = svlen; }
            else if (sf == 3) {
              size_t nn = svlen < MESH_NAME_LEN - 1 ? svlen : MESH_NAME_LEN - 1;
              memcpy(cname, sv, nn); cname[nn] = '\0';
            }
            j += svlen;          // the CLAMPED length: `sl` is attacker-chosen and unbounded
          } else if (sw == 0) { size_t jj = j; meshPbVarint(sub, sublen, &jj); j = jj; }
          else if (sw == 5) j += 4; else if (sw == 1) j += 8; else break;
        }
        // Add channels that have a name and a full key. Skip the primary/default
        // (empty name and/or the 1-byte default-key shorthand).
        if (cname[0] && (pskLen == 16 || pskLen == 32)) {
          if (addChannel(cname, psk, (uint8_t)pskLen)) added++;
        }
      }
      i += sublen;               // the CLAMPED length; `l` is attacker-chosen and may wrap
    } else if (wire == 0) { meshPbVarint(data, len, &i); }
    else if (wire == 5) i += 4; else if (wire == 1) i += 8; else break;
  }
  if (added) {
    saveChannels();
  }
  log_i("Mesh: applied channel link, %d channel(s) added", added);
  return added;
}

#define MESH_CHAN_PATH   "/meshchan.bin"
#define MESH_CHAN_MAGIC  0x314e4843u   // "CHN1"
#define MESH_CHAN_VER    1

void MeshtasticService::saveChannels() {
  File f = SPIFFS.open(MESH_CHAN_PATH, "w");
  if (!f) {
    return;
  }
  uint32_t magic = MESH_CHAN_MAGIC;
  uint16_t ver = MESH_CHAN_VER;
  uint16_t cs = (uint16_t)sizeof(MeshChannel);
  f.write((const uint8_t*)&magic, 4);
  f.write((const uint8_t*)&ver, 2);
  f.write((const uint8_t*)&cs, 2);
  int32_t n = channelCount;
  f.write((const uint8_t*)&n, 4);
  for (int i = 0; i < channelCount; i++) {
    f.write((const uint8_t*)&channels[i], sizeof(MeshChannel));
  }
  f.close();
}

void MeshtasticService::loadChannels() {
  File f = SPIFFS.open(MESH_CHAN_PATH, "r");
  if (!f) {
    return;                              // keep the default channel
  }
  uint32_t magic = 0; uint16_t ver = 0, cs = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != MESH_CHAN_MAGIC) { f.close(); return; }
  f.read((uint8_t*)&ver, 2);
  f.read((uint8_t*)&cs, 2);
  if (ver != MESH_CHAN_VER || cs != sizeof(MeshChannel)) { f.close(); return; }
  int32_t n = 0;
  if (f.read((uint8_t*)&n, 4) != 4) { f.close(); return; }
  if (n < 1) n = 1;
  if (n > MESH_MAX_CHANNELS) n = MESH_MAX_CHANNELS;
  channelCount = 0;
  for (int i = 0; i < n; i++) {
    MeshChannel c;
    if (f.read((uint8_t*)&c, sizeof(c)) != sizeof(c)) break;
    channels[channelCount++] = c;
  }
  f.close();
  if (channelCount == 0) {
    initDefaultChannel();
  }
}

bool MeshtasticService::sendChannelMessage(uint8_t channelHash, const char* text) {
  if (!text || !text[0]) {
    return false;
  }
  const MeshChannel* ch = findChannelByHash(channelHash);
  if (!ch) {
    return false;
  }
#ifdef MESHTASTIC_PHY
  /* No want_ack: a broadcast's acknowledgement is implicit (see the rebroadcast hook in
   * handleRx). Asking for one adds a header bit, no information, and a chance that stock
   * answers with a routing packet we did not need. */
  bool ok = meshTxText(myNodeNum, MESH_ADDR_BROADCAST_ONAIR, text, ch);
  uint32_t txId = s_lastTxPacketId;
  if (ok) {
    replayCapture(myNodeNum, ch, text);   // our own sends are history COVEY may have missed
    storeMessage(myNodeNum, MESH_BROADCAST_ADDR, channelHash, text, true);   // show it locally
    notePendingAck(txId, lastStoredTimeMs);
  }
  return ok;
#else
  storeMessage(myNodeNum, MESH_BROADCAST_ADDR, channelHash, text, true);
  log_i("Mesh TX (stub) channel 0x%02X: %s", channelHash, text);
  return true;
#endif
}

bool MeshtasticService::sendDirectMessage(uint32_t destNode, const char* text) {
  if (!text || !text[0] || destNode == 0 || channelCount == 0) {
    return false;
  }
  const MeshChannel* ch = &channels[0];              // DMs go on the primary channel
#ifdef MESHTASTIC_PHY
  /* PKC first: a 2.5+ peer DROPS the legacy channel-encrypted DM form on
   * receive ("Rejecting legacy DM"), so when we know the peer's key we MUST
   * send PKI. This runs at GUI depth, where the ~3 KB X25519 derive is not
   * allowed — so: cache hit sends now; known key but cold cache QUEUES for
   * loop() (goes out next tick, imperceptible); no key falls back to legacy,
   * which only a pre-2.5 peer will accept. */
  const MeshNode* peer = findNode(destNode);
  if (peer && (peer->pkiFlags & MESH_NODE_HAS_KEY) && pkiReady) {
    uint8_t skey[MESH_KEY_LEN];
    if (pkiKeyCached(destNode, skey)) {
      bool ok = meshTxDataPki(myNodeNum, destNode, MESH_PORT_TEXT_MESSAGE,
                              (const uint8_t*)text, strlen(text), skey, 0, true);
      uint32_t txId = s_lastTxPacketId;
      if (ok) {
        storeMessage(myNodeNum, destNode, 0x00, text, true);
        notePendingAck(txId, lastStoredTimeMs);
      }
      return ok;
    }
    for (int i = 0; i < 2; i++) {
      if (!pendingDm[i].active) {
        pendingDm[i].dest = destNode;
        strlcpy(pendingDm[i].text, text, sizeof(pendingDm[i].text));
        pendingDm[i].active = true;
        storeMessage(myNodeNum, destNode, 0x00, text, true);   // echo now; TX next tick
        pendingDm[i].msgTimeMs = lastStoredTimeMs;             // receipt follows it there
        return true;
      }
    }
    return false;                                    // queue full — refuse honestly
  }
  bool ok = meshTxText(myNodeNum, destNode, text, ch, true);
  uint32_t txId = s_lastTxPacketId;
  if (ok) {
    storeMessage(myNodeNum, destNode, ch->hash, text, true);
    notePendingAck(txId, lastStoredTimeMs);
    /* Visible on serial because a modern peer will NEVER show this message —
     * its firmware refuses legacy DMs. The fix is hearing their NodeInfo
     * (running `pki` shows who has keys). */
    log_e("MESH DM to !%08x sent LEGACY (no key known) - a 2.5+ node will drop it",
          (unsigned)destNode);
  }
  return ok;
#else
  storeMessage(myNodeNum, destNode, ch->hash, text, true);
  log_i("Mesh TX (stub) DM to 0x%08X: %s", destNode, text);
  return true;
#endif
}

/* ── Mesh history replay (docs/replay-spec.md) ──────────────────────────────
 *
 * The phone remembers the channel texts it hears (and sends); COVEY, blind
 * while its radio is lent or off, asks `RPL?` on the booksync channel and the
 * ring answers — original timestamps, oldest first, one packet per
 * REPLAY_TX_GAP_MS so a reply never storms the band. The wire format lives in
 * replay_proto.{h,cpp}, pinned byte-for-byte to COVEY's replay.py by
 * tests/test_replay.cpp. DMs never enter the ring: the spec's honest rule is
 * that replay recovers what any channel member could already read. */

void MeshtasticService::replayCapture(uint32_t sender, const MeshChannel* ch, const char* text) {
  if (!replayRing || !ch || !text || !text[0]) {
    return;
  }
  if (!ntpClock.isTimeKnown()) {
    return;                     // an entry without real time can serve no window
  }
  /* When this ring's coverage BEGAN. Stamped on the first packet heard after
   * the clock locks — before the machine-traffic filter, because hearing
   * anything proves we were listening. Ring fullness was the wrong proxy: the
   * ring is empty after every reboot, which is exactly the case the gap flag
   * exists for ("reboot clears it; the reply says so"). */
  if (replayCoverFrom == 0) {
    replayCoverFrom = (uint32_t)ntpClock.getExactUtcTime();
  }
  // Machine traffic is not conversation: none of it belongs in history.
  if (bookSyncIsSyncText(text) || smsMirrorIsMirrorLine(text) || replayIsReplayText(text)) {
    return;
  }
  ReplayHeard* e = &replayRing[replayHead];
  /* getExactUTCTime, emphatically not getExactUnixTime: the latter is the
   * LOCAL-shifted epoch (sun.cpp derives the tz offset from their difference),
   * and stamping it here put every replayed record 7 hours off — measured on
   * the first live content exchange, as record ts vs the Pi's clock. */
  e->rxUnix = (uint32_t)ntpClock.getExactUtcTime();
  e->sender = sender;
  strlcpy(e->chan, ch->name, sizeof(e->chan));
  strlcpy(e->text, text, sizeof(e->text));
  replayHead = (replayHead + 1) % REPLAY_RING_CAP;
  if (replayCount < REPLAY_RING_CAP) {
    replayCount++;
  }
}

int MeshtasticService::replayPendingPackets() const {
  return replayPktCount - replayPktNext;
}

void MeshtasticService::replayHandleRequest(const MeshChannel* ch, const char* text) {
  if (!replayRing || !ch) {
    return;
  }
  uint32_t t1, t2;
  int maxn;
  if (!replayParseRequest(text, &t1, &t2, &maxn)) {
    return;                     // records/summaries we overhear land here too: not ours
  }
  // Build the whole reply now; a fresh request restarts the queue (latest wins).
  replayPktCount = replayPktNext = 0;
  replayChanHash = ch->hash;
  int packed = 0;
  bool gap = false;
  char line[REPLAY_PKT_STRIDE];
  char* cur = NULL;
  size_t curLen = 0;
  for (int i = 0; i < replayCount; i++) {
    const ReplayHeard* e =
        &replayRing[(replayHead - replayCount + i + 2 * REPLAY_RING_CAP) % REPLAY_RING_CAP];
    if (e->rxUnix < t1 || e->rxUnix > t2) {
      continue;
    }
    if (packed >= maxn) {
      gap = true;               // max clipped the window: say so, don't pretend
      continue;
    }
    const int n = replayFormatRecord(line, sizeof(line), e->rxUnix, e->sender, e->chan, e->text);
    if (n < 0) {
      continue;
    }
    if (!cur || curLen + 1 + (size_t)n > REPLAY_PACKET_BUDGET) {
      if (replayPktCount >= REPLAY_MAX_PKTS - 1) {    // one slot stays reserved for the summary
        gap = true;
        break;
      }
      cur = replayPkts + (size_t)replayPktCount * REPLAY_PKT_STRIDE;
      cur[0] = '\0';
      curLen = 0;
      replayPktCount++;
    }
    if (curLen) {
      cur[curLen++] = '\n';
    }
    memcpy(cur + curLen, line, (size_t)n + 1);
    curLen += (size_t)n;
    packed++;
  }
  /* Honest coverage, two ways it can fall short: we were not listening yet
   * (boot/NTP — the ring is EMPTY then, which fullness never catches), or the
   * ring has since evicted (head == the oldest slot when full). */
  if (replayCoverFrom == 0 || t1 < replayCoverFrom) {
    gap = true;
  }
  if (replayCount == REPLAY_RING_CAP && t1 < replayRing[replayHead].rxUnix) {
    gap = true;
  }
  char sum[32];
  const int sn = replayFormatSummary(sum, sizeof(sum), packed, gap);
  if (sn > 0) {
    if (cur && curLen + 1 + (size_t)sn <= REPLAY_PACKET_BUDGET) {
      cur[curLen++] = '\n';
      memcpy(cur + curLen, sum, (size_t)sn + 1);
      curLen += (size_t)sn;
    } else if (replayPktCount < REPLAY_MAX_PKTS) {
      memcpy(replayPkts + (size_t)replayPktCount * REPLAY_PKT_STRIDE, sum, (size_t)sn + 1);
      replayPktCount++;
    }
  }
  replayNextTxMs = millis();    // first packet leaves on the next loop pass
  replayServedMs = millis();
  replayServedN = packed;
  /* log_e on purpose: only log_e is compiled into this build, and a served
   * replay is rare enough to deserve its one line of serial evidence. */
  log_e("REPLAY: [%u..%u] max %d -> %d record(s) in %d packet(s)%s",
        (unsigned)t1, (unsigned)t2, maxn, packed, replayPktCount, gap ? " GAP" : "");
}

void MeshtasticService::replayPump() {
  if (!replayRing || replayPktNext >= replayPktCount) {
    return;
  }
  const uint32_t now = millis();
  if ((int32_t)(now - replayNextTxMs) < 0) {
    return;
  }
  const MeshChannel* ch = findChannelByHash(replayChanHash);
  if (!ch) {                    // the channel vanished mid-reply (chan edit): drop the rest
    replayPktCount = replayPktNext = 0;
    return;
  }
#ifdef MESHTASTIC_PHY
  /* QUIET tx — meshTxText directly, never sendChannelMessage: protocol
   * traffic must not local-echo into this phone's own chat list. */
  meshTxText(myNodeNum, MESH_ADDR_BROADCAST_ONAIR,
             replayPkts + (size_t)replayPktNext * REPLAY_PKT_STRIDE, ch);
#endif
  replayPktNext++;
  replayNextTxMs = now + REPLAY_TX_GAP_MS;
}

/* ── Neighbour info (portnum 71) ────────────────────────────────────────────
 *
 * Nick's ask: a toggle, because knowing who hears whom is how you build a mesh
 * map while hunting. Two halves — remember who reaches us with no relay, and
 * (when switched on) say so on a private channel.
 *
 * OFF BY DEFAULT and long-interval by design: this is pure airtime spend on a
 * band shared with everyone else's traffic, and the stock module's own docs put
 * its floor at four hours. */

void MeshtasticService::neighborHeard(uint32_t node, int8_t snr, bool direct) {
  if (node == 0 || node == myNodeNum) {
    return;
  }
  const uint32_t now = millis();
  for (int i = 0; i < nbrCount; i++) {
    if (nbr[i].nodeNum == node) {
      if (direct) {
        nbr[i].snr = snr;
        nbr[i].heardMs = now;
      }
      /* A node heard only via a relay does NOT refresh the entry and never
       * creates one: it is not a neighbour, and an edge drawn to it would be
       * a road on the map that does not exist. */
      return;
    }
  }
  if (!direct) {
    return;
  }
  if (nbrCount < (int)(sizeof(nbr) / sizeof(nbr[0]))) {
    nbr[nbrCount].nodeNum = node;
    nbr[nbrCount].snr = snr;
    nbr[nbrCount].heardMs = now;
    nbrCount++;
    return;
  }
  // Full: evict the stalest, which is the one least likely to still be in earshot.
  int oldest = 0;
  for (int i = 1; i < nbrCount; i++) {
    if ((int32_t)(nbr[i].heardMs - nbr[oldest].heardMs) < 0) {
      oldest = i;
    }
  }
  nbr[oldest].nodeNum = node;
  nbr[oldest].snr = snr;
  nbr[oldest].heardMs = now;
}

bool MeshtasticService::getDirectNeighbor(int i, uint32_t* node, int* snr, uint32_t* ageMs) const {
  if (i < 0 || i >= nbrCount) {
    return false;
  }
  if (node) {
    *node = nbr[i].nodeNum;
  }
  if (snr) {
    *snr = nbr[i].snr;
  }
  if (ageMs) {
    *ageMs = millis() - nbr[i].heardMs;
  }
  return true;
}

/* Where a neighbour announce goes: the first PRIVATE channel, never the
 * primary. The primary here is stock LongFast with the public key, and
 * broadcasting our mesh topology in the clear tells any stranger with a radio
 * exactly who is out here and how well they hear each other. (Stock firmware
 * reaches the same conclusion from the other end: its module refuses to
 * transmit over LoRa on a default-key channel at all.) */
/* `booksync` and `smsmirror` carry MACHINE traffic between these two devices
 * and have no human audience: a neighbour map broadcast there is airtime spent
 * where nobody is looking. Nick's answer when asked was "howe and hunt group
 * and so on" — the channels people are actually on. */
bool MeshtasticService::channelIsMachine(const MeshChannel* ch) const {
  return ch && (strcasecmp(ch->name, "booksync") == 0 ||
                strcasecmp(ch->name, "smsmirror") == 0);
}

const MeshChannel* MeshtasticService::neighborChannel() const {
  for (int i = 1; i < channelCount; i++) {
    if (channels[i].keyLen > 0 && !channelIsMachine(&channels[i])) {
      return &channels[i];
    }
  }
  return NULL;
}

const char* MeshtasticService::neighborChannelName() const {
  const MeshChannel* ch = neighborChannel();
  return ch ? ch->name : NULL;
}

bool MeshtasticService::announceNeighborsOn(const MeshChannel* ch) {
  if (!ch) {
    return false;
  }
  /* Only neighbours heard recently enough to still mean something. An hour of
   * silence from a node in the woods means it moved, slept, or died; claiming
   * it as an edge would be a guess dressed as a measurement. */
  NeighborEntry list[16];
  int n = 0;
  const uint32_t now = millis();
  const int cap = neighborInfoCapacity(MESH_TEXT_LEN - 8, myNodeNum, nbrIntervalSecs);
  for (int i = 0; i < nbrCount && n < cap && n < (int)(sizeof(list) / sizeof(list[0])); i++) {
    if ((uint32_t)(now - nbr[i].heardMs) > 3600000UL) {
      continue;
    }
    list[n].nodeNum = nbr[i].nodeNum;
    list[n].snr = (float)nbr[i].snr;
    n++;
  }
  uint8_t payload[MESH_TEXT_LEN];
  const int len = neighborInfoEncode(payload, sizeof(payload), myNodeNum, nbrIntervalSecs,
                                     list, n);
  if (len < 0) {
    log_e("NEIGHBOR: encode failed (%d neighbours)", n);
    return false;
  }
#ifdef MESHTASTIC_PHY
  /* wantResponse FALSE: a map update that also demanded every hearer answer
   * would turn one node's housekeeping into a mesh-wide storm — the same rule
   * the periodic NodeInfo beacon follows. */
  const bool ok = meshTxData(myNodeNum, MESH_ADDR_BROADCAST_ONAIR, MESH_PORT_NEIGHBORINFO,
                             payload, (size_t)len, false, ch->key, ch->keyLen, ch->hash);
#else
  const bool ok = true;
#endif
  nbrLastTxMs = millis();
  nbrLastTxCount = n;
  log_e("NEIGHBOR: announced %d neighbour(s) on '%s' (%d bytes)%s",
        n, ch->name, len, ok ? "" : " - TX FAILED");
  return ok;
}

/* Announce on EVERY private channel right now, blocking. Only the bench calls
 * this (serial `nbr now`); the scheduled path drips one channel per pass
 * instead, because three LongFast sends back to back stall the superloop. */
int MeshtasticService::announceNeighborsNow() {
  int sent = 0;
  for (int i = 1; i < channelCount && i < 8; i++) {
    if (channels[i].keyLen > 0 && !channelIsMachine(&channels[i])) {
      if (announceNeighborsOn(&channels[i])) {
        sent++;
      }
      delay(50);
    }
  }
  return sent;
}

void MeshtasticService::setNeighborInterval(uint32_t secs) {
  nbrIntervalSecs = secs;
  nbrNextTxMs = 0;              // re-arm: a fresh choice waits a full period
  Preferences p;
  p.begin("wpmesh", false);
  p.putULong("nbrint", secs);
  p.end();
  log_i("Mesh: neighbour announce %s (%lus)", secs ? "ON" : "OFF", (unsigned long)secs);
}
