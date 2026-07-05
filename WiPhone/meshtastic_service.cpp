/*
 * meshtastic_service.cpp — WiPhone Meshtastic integration (Phase 3 shell)
 *
 * Stubbed radio: no hardware access yet. The public API is complete so the UI
 * can be built and exercised end-to-end. The real SX1276/Meshtastic PHY will
 * replace the stub internals (storeMessage on RX, actual TX in send*) later.
 */

#include "meshtastic_service.h"
#include "config.h"            // MESHTASTIC_PHY toggle
#include <Preferences.h>      // NVS-backed node name persistence
#include <SPIFFS.h>           // node + message history persistence

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
#define MESH_PORT_NODEINFO      4

// Default hop limit for packets we originate (Meshtastic default is 3).
#define MESH_TX_HOP_LIMIT       3

// Minimal parser for a decrypted Meshtastic Data protobuf. Returns field 1
// (portnum, varint) and, via the out params, field 2 (payload bytes). The
// payload meaning depends on portnum: UTF-8 text for TEXT_MESSAGE, a nested
// User protobuf for NODEINFO, etc. Bounds-checked against `dlen`.
static int meshParseData(const uint8_t* data, size_t dlen,
                         const uint8_t** payloadOut, size_t* payloadLenOut) {
  int portnum = -1;
  const uint8_t* payload = NULL;
  size_t payloadLen = 0;
  size_t i = 0;
  while (i < dlen) {
    uint8_t tag = data[i++];
    uint8_t field = tag >> 3;
    uint8_t wire = tag & 0x07;
    if (wire == 0) {                       // varint
      uint32_t v = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { v |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { v |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 1) portnum = (int)v;
    } else if (wire == 2) {                // length-delimited
      uint32_t l = 0; int shift = 0;
      while (i < dlen && (data[i] & 0x80)) { l |= (uint32_t)(data[i] & 0x7f) << shift; shift += 7; i++; }
      if (i < dlen) { l |= (uint32_t)(data[i] & 0x7f) << shift; i++; }
      if (field == 2) { payload = data + i; payloadLen = l; }
      i += l;
    } else if (wire == 5) { i += 4; }
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
static bool meshParseUserName(const uint8_t* data, size_t dlen, char* out, size_t outCap) {
  const uint8_t* longName = NULL;  size_t longLen = 0;
  const uint8_t* shortName = NULL; size_t shortLen = 0;
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

// Build a Data protobuf { portnum, payload, [want_response] } and transmit it as
// an encrypted Meshtastic packet on the given channel (key + hash).
static bool meshTxData(uint32_t sender, uint32_t dest, int portnum,
                       const uint8_t* payload, size_t payloadLen, bool wantResponse,
                       const uint8_t* key, uint8_t keyLen, uint8_t channelHash) {
  uint8_t data[8 + MESH_PHY_MAX_PAYLOAD];
  size_t d = 0;
  data[d++] = 0x08;                          // field 1 portnum (varint)
  data[d++] = (uint8_t)portnum;
  d += pbBytes(data + d, 2, payload, payloadLen);   // field 2 payload
  if (wantResponse) {
    data[d++] = 0x18;                        // field 3 want_response (varint)
    data[d++] = 0x01;
  }

  uint32_t packetId = esp_random();
  if (packetId == 0) {
    packetId = 1;
  }

  MeshPacketHeader hdr;
  hdr.dest        = dest;
  hdr.sender      = sender;
  hdr.packetId    = packetId;
  hdr.flags       = (s_hopLimit & MESH_FLAGS_HOP_LIMIT_MASK) |
                    (s_hopLimit << MESH_FLAGS_HOP_START_SHIFT);
  hdr.channelHash = channelHash;
  hdr.nextHop     = 0;
  hdr.relayNode   = (uint8_t)(sender & 0xFF);   // originator is the first relay

  uint8_t pkt[MESH_HEADER_LEN + 8 + MESH_PHY_MAX_PAYLOAD];
  memcpy(pkt, &hdr, MESH_HEADER_LEN);
  if (!meshCryptCtr(key, keyLen, sender, packetId, data, d, pkt + MESH_HEADER_LEN)) {
    return false;
  }
  size_t pktLen = MESH_HEADER_LEN + d;
  log_i("Mesh TX port=%d to 0x%08X ch=0x%02X id=0x%08X (%uB)",
        portnum, dest, channelHash, packetId, (unsigned)pktLen);
  return meshPhy.send(pkt, (uint8_t)pktLen);
}

static bool meshTxText(uint32_t sender, uint32_t dest, const char* text,
                       const MeshChannel* ch) {
  size_t textLen = strlen(text);
  if (textLen == 0 || textLen > MESH_TEXT_LEN - 1 || !ch) {
    return false;
  }
  return meshTxData(sender, dest, MESH_PORT_TEXT_MESSAGE, (const uint8_t*)text, textLen,
                    false, ch->key, ch->keyLen, ch->hash);
}

// Broadcast our NodeInfo: a User protobuf { id, long_name, short_name }.
// wantResponse solicits other nodes to reply with their own NodeInfo.
static bool meshTxNodeInfo(uint32_t sender, const char* longName, const char* shortName,
                           bool wantResponse, const MeshChannel* ch) {
  if (!ch) {
    return false;
  }
  char id[12];
  snprintf(id, sizeof(id), "!%08x", sender);

  uint8_t user[96];
  size_t u = 0;
  u += pbString(user + u, 1, id);            // field 1 id
  u += pbString(user + u, 2, longName);      // field 2 long_name
  u += pbString(user + u, 3, shortName);     // field 3 short_name

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
    myNodeNum(0), recentPktPos(0), dbDirty(false), lastSaveMs(0), initialized(false) {
  messages = NULL;
  msgCount = 0;
  channelCount = 0;
  myHopLimit = 3;
  memset(nodes, 0, sizeof(nodes));
  memset(channels, 0, sizeof(channels));
  memset(recentPktIds, 0, sizeof(recentPktIds));
  memset(rebroadcast, 0, sizeof(rebroadcast));
  myLongName[0] = '\0';
  myShortName[0] = '\0';
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
  deriveShortName();
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
  deriveShortName();

  Preferences prefs;
  prefs.begin("wpmesh", false);                // read-write
  prefs.putString("lname", myLongName);
  prefs.end();

  announceNodeInfo(true);                       // let the mesh learn the new name
  log_i("Mesh node renamed to '%s' (%s)", myLongName, myShortName);
}

void MeshtasticService::announceNodeInfo(bool wantResponse) {
#ifdef MESHTASTIC_PHY
  if (radioState == MESH_RADIO_READY && channelCount > 0) {
    meshTxNodeInfo(myNodeNum, myLongName, myShortName, wantResponse, &channels[0]);
  }
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
  memset(nodes, 0, sizeof(nodes));
  upsertNode(myNodeNum, myLongName);          // keep this node in the list
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

  // Allocate the message store in PSRAM (falls back to internal RAM if needed).
  messages = (MeshMessage*)ps_malloc((size_t)MESH_MSG_CAP * sizeof(MeshMessage));
  if (!messages) {
    messages = (MeshMessage*)malloc((size_t)MESH_MSG_CAP * sizeof(MeshMessage));
  }
  msgCount = 0;
  if (!messages) {
    log_e("MeshtasticService: message buffer alloc FAILED");
  }

  // Load our editable node name (from NVS, or a default derived from the id).
  loadMyName();

  // Channels: start with LongFast, then restore any saved custom channels.
  initDefaultChannel();
  loadChannels();

  // Restore persisted nodes + message history (SPIFFS is mounted by now).
  loadDb();

  // Always register ourselves as a node, under our own name.
  upsertNode(myNodeNum, myLongName);

#ifdef MESHTASTIC_PHY
  // Real radio: bring up the SX1276 in Meshtastic LongFast RX mode.
  if (meshPhy.begin()) {
    radioState = MESH_RADIO_READY;
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
  if (dbDirty && (millis() - lastSaveMs > MESH_SAVE_DEBOUNCE_MS)) {
    saveDb();
    dbDirty = false;
    lastSaveMs = millis();
  }

#ifdef MESHTASTIC_PHY
  // Flood routing: send at most one due rebroadcast per tick (send blocks).
  for (int i = 0; i < 4; i++) {
    if (rebroadcast[i].active && (int32_t)(millis() - rebroadcast[i].dueMs) >= 0) {
      meshPhy.send(rebroadcast[i].data, rebroadcast[i].len);
      rebroadcast[i].active = false;
      break;
    }
  }

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

  // Ignore packets we originated that the mesh rebroadcast back to us.
  if (hdr.sender == myNodeNum) {
    return false;
  }
  // Drop duplicate rebroadcasts of the same packet (mesh flooding).
  if (seenPacketId(hdr.packetId)) {
    return false;
  }

  // Every heard sender is a known node.
  MeshNode* n = upsertNode(hdr.sender, NULL);
  if (n) {
    n->snr = snr;
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

  if (payloadLen > 0 && ch) {
    // Known channel: decrypt with its key (AES-128 or AES-256) and decode.
    uint8_t dec[MESH_PHY_MAX_PAYLOAD];
    if (meshCryptCtr(ch->key, ch->keyLen, hdr.sender, hdr.packetId, buf + MESH_HEADER_LEN, payloadLen, dec)) {
      const uint8_t* pl = NULL;
      size_t plLen = 0;
      int portnum = meshParseData(dec, payloadLen, &pl, &plLen);

      if (portnum == MESH_PORT_TEXT_MESSAGE && pl && plLen) {
        char text[MESH_TEXT_LEN];
        size_t nt = plLen < sizeof(text) - 1 ? plLen : sizeof(text) - 1;
        memcpy(text, pl, nt);
        text[nt] = '\0';
        storeMessage(hdr.sender, toInternal, hdr.channelHash, text, false);   // real text!
        log_i("Mesh TEXT from 0x%08X on ch '%s': %s", hdr.sender, ch->name, text);
        return true;                       // signal the UI to refresh

      } else if (portnum == MESH_PORT_NODEINFO && pl && plLen) {
        // Learn the sender's friendly name for the Chats/Nodes lists.
        char name[MESH_NAME_LEN];
        if (meshParseUserName(pl, plLen, name, sizeof(name))) {
          upsertNode(hdr.sender, name);
          log_i("Mesh NodeInfo 0x%08X: %s", hdr.sender, name);
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

  dbDirty = true;         // new message: persist soon
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
      if (name && name[0] && strcmp(nodes[i].name, name) != 0) {
        strlcpy(nodes[i].name, name, sizeof(nodes[i].name));
        dbDirty = true;   // name changed: persist soon
      }
      return &nodes[i];   // plain "heard" updates don't trigger a save
    }
  }
  // New node.
  if (nodeCount >= MESH_MAX_NODES) {
    return NULL;      // table full; eviction policy added later
  }
  MeshNode& n = nodes[nodeCount];
  n.nodeNum = nodeNum;
  n.lastHeardMs = millis();
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
#define MESH_DB_MAGIC     0x314D5057u   // "WPM1"
#define MESH_DB_VERSION   1

void MeshtasticService::saveDb() {
  File f = SPIFFS.open(MESH_DB_PATH, "w");
  if (!f) {
    log_e("mesh: saveDb open failed");
    return;
  }
  uint32_t magic = MESH_DB_MAGIC;
  uint16_t ver = MESH_DB_VERSION;
  uint16_t ns = (uint16_t)sizeof(MeshNode);
  uint16_t ms = (uint16_t)sizeof(MeshMessage);
  f.write((const uint8_t*)&magic, 4);
  f.write((const uint8_t*)&ver, 2);
  f.write((const uint8_t*)&ns, 2);
  f.write((const uint8_t*)&ms, 2);

  int32_t nc = nodeCount;
  f.write((const uint8_t*)&nc, sizeof(nc));
  for (int i = 0; i < nodeCount; i++) {
    f.write((const uint8_t*)&nodes[i], sizeof(MeshNode));
  }

  int32_t mc = msgCount;
  f.write((const uint8_t*)&mc, sizeof(mc));
  for (int i = msgCount - 1; i >= 0; i--) {           // oldest -> newest
    const MeshMessage* m = getMessage(i);
    if (m) {
      f.write((const uint8_t*)m, sizeof(MeshMessage));
    }
  }
  f.close();
  log_i("mesh: saved %d nodes, %d msgs", nodeCount, msgCount);
}

void MeshtasticService::loadDb() {
  File f = SPIFFS.open(MESH_DB_PATH, "r");
  if (!f) {
    return;                             // first boot: nothing saved yet
  }
  uint32_t magic = 0;
  uint16_t ver = 0, ns = 0, ms = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != MESH_DB_MAGIC) { f.close(); return; }
  f.read((uint8_t*)&ver, 2);
  f.read((uint8_t*)&ns, 2);
  f.read((uint8_t*)&ms, 2);
  // Struct-layout guard: ignore stale data written by a different build.
  if (ver != MESH_DB_VERSION || ns != sizeof(MeshNode) || ms != sizeof(MeshMessage)) {
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
    if (f.read((uint8_t*)&n, sizeof(n)) != sizeof(n)) break;
    nodes[nodeCount++] = n;
  }

  int32_t mc = 0;
  if (messages && f.read((uint8_t*)&mc, sizeof(mc)) == sizeof(mc)) {
    if (mc < 0) mc = 0;
    if (mc > MESH_MSG_CAP) mc = MESH_MSG_CAP;
    msgCount = 0;
    for (int i = 0; i < mc; i++) {       // stored oldest -> newest
      MeshMessage m;
      if (f.read((uint8_t*)&m, sizeof(m)) != sizeof(m)) break;
      messages[msgCount++] = m;
    }
  }
  f.close();
  log_i("mesh: loaded %d nodes, %d msgs", nodeCount, msgCount);
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

const MeshNode* MeshtasticService::getNode(int index) const {
  if (index < 0 || index >= nodeCount) {
    return NULL;
  }
  return &nodes[index];
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
      size_t sublen = (i + l <= (size_t)len) ? l : ((size_t)len - i);
      if (field == 1) {
        const uint8_t* psk = NULL; size_t pskLen = 0;
        char cname[MESH_NAME_LEN] = {0};
        size_t j = 0;
        while (j < sublen) {
          uint8_t st = sub[j++]; int sf = st >> 3, sw = st & 7;
          if (sw == 2) {
            size_t jj = j; uint32_t sl = meshPbVarint(sub, sublen, &jj); j = jj;
            const uint8_t* sv = sub + j; size_t svlen = (j + sl <= sublen) ? sl : (sublen - j);
            if (sf == 2) { psk = sv; pskLen = svlen; }
            else if (sf == 3) {
              size_t nn = svlen < MESH_NAME_LEN - 1 ? svlen : MESH_NAME_LEN - 1;
              memcpy(cname, sv, nn); cname[nn] = '\0';
            }
            j += sl;
          } else if (sw == 0) { size_t jj = j; meshPbVarint(sub, sublen, &jj); j = jj; }
          else if (sw == 5) j += 4; else if (sw == 1) j += 8; else break;
        }
        // Add channels that have a name and a full key. Skip the primary/default
        // (empty name and/or the 1-byte default-key shorthand).
        if (cname[0] && (pskLen == 16 || pskLen == 32)) {
          if (addChannel(cname, psk, (uint8_t)pskLen)) added++;
        }
      }
      i += l;
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
  bool ok = meshTxText(myNodeNum, MESH_ADDR_BROADCAST_ONAIR, text, ch);
  if (ok) {
    storeMessage(myNodeNum, MESH_BROADCAST_ADDR, channelHash, text, true);   // show it locally
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
  bool ok = meshTxText(myNodeNum, destNode, text, ch);
  if (ok) {
    storeMessage(myNodeNum, destNode, ch->hash, text, true);
  }
  return ok;
#else
  storeMessage(myNodeNum, destNode, ch->hash, text, true);
  log_i("Mesh TX (stub) DM to 0x%08X: %s", destNode, text);
  return true;
#endif
}
