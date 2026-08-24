/*
 * app_meshtastic.cpp — WiPhone Meshtastic UI (multi-channel conversations)
 *
 * Chats lists one thread per known channel (LongFast + any imported custom
 * channels) plus one thread per DM peer. Viewing a message that contains a
 * Meshtastic channel link lets you apply it ("Apply link" = OK in the viewer).
 */

#include "app_meshtastic.h"
#include "meshtastic_service.h"
#include "mesh_pos.h"           // distance/bearing for the Nodes and Places lists
#include "clock.h"              // ntpClock, for the Sun screen
#include "sun_times.h"          // legal light math (host-tested; the serial `sun` core)

#ifdef USER_SERIAL              // the GPS toggle's plumbing (WiPhone.ino owns the flag)
#include <Preferences.h>
extern bool gGpsNmea;
extern void gpsApplyBaud(bool gpsOn);   // WiPhone.ino: the GPS and the GUI path differ in baud
#endif

// Main-menu option keys
#define MESH_KEY_CHATS      1
#define MESH_KEY_NODES      2
#define MESH_KEY_STATUS     3
#define MESH_KEY_MYNODE     4
#define MESH_KEY_PLACES     5
#define MESH_KEY_SUN        6
// Places > one waypoint:
#define MESH_KEY_SETREF     1
#define MESH_KEY_IMHERE     2

// "My node" screen option keys
#define MESH_MYNODE_EDIT       1
#define MESH_MYNODE_REQUEST    2
#define MESH_MYNODE_HOPLIMIT   7
#define MESH_MYNODE_EDITSHORT  8      // ⚠ keys must be UNIQUE; 3 and 4 are the info rows
#define MESH_MYNODE_CLEARMSGS  5
#define MESH_MYNODE_CLEARNODES 6
#define MESH_MYNODE_GPS        10     // woods plate: route the user UART to NMEA
#define MESH_MYNODE_NEIGHBOR   11     // neighbour-info announce: off / 1h / 4h

// Thread menu: "Write" row key (kept above any message index, which is idx+1).
#define MESH_KEY_WRITE      2000000

MeshtasticApp::MeshtasticApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer)
  : WindowedApp(disp, state, header, footer) {
  log_d("create MeshtasticApp");
  appState = MESH_MAIN;
  returnState = MESH_MAIN;
  menu = NULL;
  textArea = NULL;
  threadIsChannel = true;
  threadChannelHash = 0;
  threadPeer = 0;
  viewMsgIndex = -1;
  pendingClear = 0;
  chatCount = 0;
  enterState(MESH_MAIN);
}

MeshtasticApp::~MeshtasticApp() {
  log_d("destroy MeshtasticApp");
  freeWidgets();
}

void MeshtasticApp::freeWidgets() {
  if (menu) {
    delete menu;
    menu = NULL;
  }
  if (textArea) {
    delete textArea;
    textArea = NULL;
  }
}

MenuWidget* MeshtasticApp::newMenu(const char* emptyMessage) {
  MenuWidget* m = new MenuWidget(0, header->height(), lcd.width(),
                                 lcd.height() - header->height() - footer->height(),
                                 emptyMessage, fonts[AKROBAT_EXTRABOLD_22], N_MENU_ITEMS, 8);
  // Green/black "battery saver" theme: white text on black, green highlight.
  m->setStyle(MenuWidget::DEFAULT_STYLE, WHITE, BLACK, BLACK, GREEN);
  return m;
}

// Human label for a node: friendly name if known, else "!hexid".
static void meshNodeLabel(uint32_t node, char* out, size_t cap) {
  const MeshNode* n = meshService.findNode(node);
  if (n && n->name[0]) {
    strlcpy(out, n->name, cap);
  } else {
    snprintf(out, cap, "!%08x", node);
  }
}

void MeshtasticApp::enterState(MeshAppState_t state) {
  appState = state;
  freeWidgets();

  switch (state) {
  case MESH_MAIN:
    header->setTitle("Meshtastic");
    footer->setButtons("Select", "Back");
    buildMainMenu();
    break;
  case MESH_CHATS:
    header->setTitle("Chats");
    footer->setButtons("Open", "Back");
    buildChats();
    break;
  case MESH_THREAD: {
    // Opening a thread marks it read; the status-bar icon stays if unread remain.
    meshService.markRead(threadIsChannel, threadChannelHash, threadPeer);
    controlState.meshUnread = (meshService.getUnreadTotal() > 0);
    if (threadIsChannel) {
      const MeshChannel* ch = meshService.findChannelByHash(threadChannelHash);
      /* Copy, don't point: setTitle stores the pointer, and ch->name lives in a
       * table applyChannelUrl can rewrite. (A stack local here was worse still —
       * the header re-read a dead frame on every repaint. Review catch.) */
      strlcpy(threadTitle, ch ? ch->name : "Channel", sizeof(threadTitle));
    } else {
      meshNodeLabel(threadPeer, threadTitle, sizeof(threadTitle));
    }
    header->setTitle(threadTitle);
    footer->setButtons("Select", "Back");
    buildThread();
    break;
  }
  case MESH_NODES:
    /* The key hint rides in the TITLE because nothing else on this screen can carry it: the
     * footer's two slots are already the OK action and Back, and `*` is neither. Starring
     * worked from the day it shipped and was still undiscoverable — nothing anywhere told you
     * the key existed. Kept to 13 characters; "Direct message" (14) is an existing title on
     * this same header, so this fits with room to spare. */
    header->setTitle("Nodes  *=star");
    footer->setButtons("Message", "Back");
    buildNodes();
    break;
  case MESH_STATUS:
    header->setTitle("Status");
    footer->setButtons("", "Back");
    buildStatus();
    break;
  case MESH_COMPOSE:
    header->setTitle(threadIsChannel ? "New message" : "Direct message");
    // "Clear", not "Cancel": the key under this label is BACK, which is backspace inside a
    // text field. Cancel is END, the button below it. See the WIPHONE_KEY_END cases below.
    footer->setButtons("Send", "Clear");
    buildCompose();
    break;
  case MESH_VIEWMSG:
    header->setTitle("Message");
    footer->setButtons("Apply link", "Back");
    break;
  case MESH_MYNODE:
    pendingClear = 0;                   // reset any pending confirm on entry
    header->setTitle("My node");
    footer->setButtons("Select", "Back");
    buildMyNode();
    break;
  case MESH_PLACES:
    header->setTitle("Places");
    footer->setButtons("Open", "Back");
    buildPlaces();
    break;
  case MESH_SUN:
    header->setTitle("Sun");
    footer->setButtons("Refresh", "Back");
    buildSun();
    break;
  case MESH_PLACE_OPTS: {
    const MeshWaypoint* w = meshService.findWaypoint(selectedWaypointId);
    strlcpy(placeTitle, w ? w->name : "Place", sizeof(placeTitle));   // copy: see header note
    header->setTitle(placeTitle);
    footer->setButtons("Select", "Back");
    buildPlaceOpts();
    break;
  }
  case MESH_EDITNAME:
    header->setTitle("Edit name");
    footer->setButtons("Save", "Clear");        // Back is backspace here; END cancels
    buildEditName();
    break;
  case MESH_EDITSHORT:
    header->setTitle("Short name");
    footer->setButtons("Save", "Clear");        // Back is backspace here; END cancels
    buildEditShortName();
    break;
  }
}

void MeshtasticApp::buildMainMenu() {
  menu = newMenu(NULL);
  char line[24];
  menu->addOption("Chats", MESH_KEY_CHATS, 1);
  snprintf(line, sizeof(line), "Nodes (%d)", meshService.getNodeCount());
  menu->addOption(line, MESH_KEY_NODES, 1);
  snprintf(line, sizeof(line), "Places (%d)", meshService.getWaypointCount());
  menu->addOption(line, MESH_KEY_PLACES, 1);
  menu->addOption("Sun & legal light", MESH_KEY_SUN, 1);
  menu->addOption("Status", MESH_KEY_STATUS, 1);
  menu->addOption("My node", MESH_KEY_MYNODE, 1);
}

void MeshtasticApp::buildChats() {
  menu = newMenu(NULL);
  chatCount = 0;
  int counts[MESH_APP_MAX_CHATS];

  uint32_t myNode = meshService.getMyNodeNum();
  int total = meshService.getMessageCount();

  // 1) One entry per configured channel (always shown so you can send).
  int nch = meshService.getChannelCount();
  for (int c = 0; c < nch && chatCount < MESH_APP_MAX_CHATS; c++) {
    const MeshChannel* ch = meshService.getChannel(c);
    if (!ch) {
      continue;
    }
    int cnt = 0;    // unread messages on this channel
    for (int i = 0; i < total; i++) {
      const MeshMessage* m = meshService.getMessage(i);
      if (m && m->to == MESH_BROADCAST_ADDR && m->channelHash == ch->hash &&
          !(m->flags & MESH_MSG_READ)) cnt++;
    }
    chatIsChannel[chatCount] = true;
    chatId[chatCount] = ch->hash;
    counts[chatCount] = cnt;
    chatCount++;
  }

  // 2) One entry per DM peer, most-recent first.
  for (int i = 0; i < total && chatCount < MESH_APP_MAX_CHATS; i++) {
    const MeshMessage* m = meshService.getMessage(i);
    if (!m || m->to == MESH_BROADCAST_ADDR) {
      continue;
    }
    uint32_t peer = (m->from == myNode) ? m->to : m->from;
    if (peer == 0) {
      continue;
    }
    bool unread = !(m->flags & MESH_MSG_READ);
    int idx = -1;
    for (int j = 0; j < chatCount; j++) {
      if (!chatIsChannel[j] && chatId[j] == peer) { idx = j; break; }
    }
    if (idx < 0) {
      chatIsChannel[chatCount] = false;
      chatId[chatCount] = peer;
      counts[chatCount] = unread ? 1 : 0;      // count unread only
      chatCount++;
    } else if (unread) {
      counts[idx]++;
    }
  }

  char line[64];
  for (int j = 0; j < chatCount; j++) {
    char name[MESH_NAME_LEN];
    if (chatIsChannel[j]) {
      const MeshChannel* ch = meshService.findChannelByHash((uint8_t)chatId[j]);
      strlcpy(name, ch ? ch->name : "?", sizeof(name));
    } else {
      meshNodeLabel(chatId[j], name, sizeof(name));
    }
    /* Unread count FIRST: trailing it, a 23-char node name ellipsized away the
     * one thing that makes a row worth opening. */
    if (counts[j] > 0) {
      snprintf(line, sizeof(line), "(%d) %s", counts[j], name);
    } else {
      strlcpy(line, name, sizeof(line));
    }
    /* Second line: the newest message in that chat, so the list answers "what's
     * new" without opening anything (and long names no longer collide with it). */
    char preview[64] = "";
    for (int i = 0; i < total; i++) {
      const MeshMessage* m = meshService.getMessage(i);     // 0 = newest
      if (!m) {
        continue;
      }
      bool match = chatIsChannel[j]
                   ? (m->to == MESH_BROADCAST_ADDR && m->channelHash == (uint8_t)chatId[j])
                   : (m->to != MESH_BROADCAST_ADDR &&
                      ((m->from == myNode) ? m->to : m->from) == chatId[j]);
      if (match) {
        snprintf(preview, sizeof(preview), "%s%s",
                 (m->flags & MESH_MSG_OUTGOING) ? "me: " : "", m->text);
        break;
      }
    }
    menu->addOption(line, preview[0] ? preview : NULL, (MenuOption::keyType)(j + 1), 1);
  }
}

void MeshtasticApp::buildThread() {
  menu = newMenu("No messages yet");
  menu->addOption("Write message...", MESH_KEY_WRITE, 1);

  uint32_t myNode = meshService.getMyNodeNum();
  int total = meshService.getMessageCount();
  char line[80];

  for (int i = 0; i < total; i++) {
    const MeshMessage* m = meshService.getMessage(i);      // 0 = newest
    if (!m) {
      continue;
    }
    bool inThread;
    if (threadIsChannel) {
      inThread = (m->to == MESH_BROADCAST_ADDR && m->channelHash == threadChannelHash);
    } else {
      inThread = (m->to != MESH_BROADCAST_ADDR &&
                  ((m->from == myNode) ? m->to : m->from) == threadPeer);
    }
    if (!inThread) {
      continue;
    }

    const char* who;
    char whoBuf[MESH_NAME_LEN];
    char meBuf[24];
    if (m->flags & MESH_MSG_OUTGOING) {
      /* ⚠ DELIVERY receipts, NOT read receipts — the wording matters and is the whole
       * reason these are words rather than ticks. Meshtastic has no concept of a human
       * having read anything; the strongest signal in the protocol is that a RADIO
       * acknowledged the packet. Saying "read" would be a claim the mesh cannot support.
       *
       * "in mesh" vs "delivered" is the other honest distinction. A DM is acknowledged by
       * the destination node itself, so delivered means delivered. A broadcast is
       * acknowledged implicitly by whoever rebroadcast it first — that proves the message
       * got into the mesh and nothing at all about who has it.
       *
       * WORDS, NOT TICKS, for a second reason: the Akrobat/OpenSans faces here are built
       * from generated bitmap glyphs and nothing anywhere in this firmware renders a
       * non-ASCII character. A checkmark would come out as a box, which is worse than no
       * marker — COVEY hit exactly this and had to DRAW its ticks instead. */
      const char* mark;
      if (m->flags & MESH_MSG_FAILED) {
        mark = "failed";
      } else if (m->flags & MESH_MSG_DELIVERED) {
        mark = (m->to == MESH_BROADCAST_ADDR) ? "in mesh" : "delivered";
      } else {
        mark = "sent";
      }
      snprintf(meBuf, sizeof(meBuf), "me - %s", mark);
      who = meBuf;
    } else {
      meshNodeLabel(m->from, whoBuf, sizeof(whoBuf));
      who = whoBuf;
    }
    /* Text on top (the thing you're scanning for), sender below in the small
     * font. The old "who: text" single line spent its width on the name and
     * clipped the words. Full text is still one OK away in the viewer. */
    strlcpy(line, m->text, sizeof(line));
    menu->addOption(line, who, (MenuOption::keyType)(i + 1), 1);   // key = global msg index + 1
  }
}

/* ⭐ Starred nodes are marked with a leading "*", and ASCII is not a compromise here — the
 * Akrobat faces are generated bitmap glyphs and nothing in this firmware renders a non-ASCII
 * character, so a real star (U+2605) would come out as a hollow box. Same finding as the
 * delivery receipts, which is why those say words rather than ticks. The "*" also names the
 * key that toggles it. */
static const char* meshStarName(const MeshNode* n, char* buf, size_t cap) {
  if (!(n->pkiFlags & MESH_NODE_FAVOURITE)) {
    return n->name;
  }
  snprintf(buf, cap, "*%s", n->name);
  return buf;
}

void MeshtasticApp::buildNodes() {
  /* Re-sort exactly here and nowhere else: this is the one moment the row numbering is
   * allowed to change, because it is the moment the rows are drawn. See getNode(). */
  meshService.refreshNodeOrder();
  menu = newMenu("No nodes heard yet");
  char line[64];
  char starBuf[MESH_NAME_LEN + 2];

  /* Distances are measured from the REFERENCE — a waypoint (Places) or the
   * user's own pin. The phone has no GPS; this is "where is everyone relative
   * to camp", which is how the question actually gets asked in the field. */
  int32_t refLat = 0, refLon = 0;
  bool haveRef = meshService.resolveReference(&refLat, &refLon, NULL, 0);

  char refName[20] = "";
  meshService.resolveReference(NULL, NULL, refName, sizeof(refName));

  int count = meshService.getNodeCount();
  for (int i = 0; i < count; i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n) {
      continue;
    }
    /* Two lines: the NAME stays whole on top, the position facts live below in
     * the smaller font — one line held both and the 240px screen cut whichever
     * mattered ("wrap, don't clip" — field request 2026-08-19). */
    if (haveRef && n->posHeardMs != 0 && n->nodeNum != meshService.getMyNodeNum()) {
      char dist[16];
      meshPosFmtDist(meshPosDistanceM(refLat, refLon, n->latI, n->lonI), dist, sizeof(dist));
      /* Age BEFORE the reference name: the name is the part that varies in
       * length, so trailing it pushed "(old)"/"(4m)" — the freshness, which is
       * what makes the distance trustworthy — off the end first. */
      /* ASCII separators only. "·" is U+00B7 — it appears in this codebase's
       * COMMENTS, never in drawn text, and nothing proves the Akrobat fonts
       * carry the glyph; a missing one renders as a hollow box. Also "min"
       * rather than "m", which would sit next to a distance ending in "m". */
      if (n->posHeardMs == 1) {          // restored from flash: age unknown
        snprintf(line, sizeof(line), "%s %s of %s, age unknown", dist,
                 meshPosCompass8(meshPosBearingDeg(refLat, refLon, n->latI, n->lonI)), refName);
      } else {
        snprintf(line, sizeof(line), "%s %s of %s, %u min ago", dist,
                 meshPosCompass8(meshPosBearingDeg(refLat, refLon, n->latI, n->lonI)), refName,
                 (unsigned)((millis() - n->posHeardMs) / 60000u));
      }
      menu->addOption(meshStarName(n, starBuf, sizeof(starBuf)), line,
                      (MenuOption::keyType)(i + 1), 1);
    } else {
      snprintf(line, sizeof(line), "!%08x%s", n->nodeNum,
               n->posHeardMs != 0 ? "" : " - no position heard");
      menu->addOption(meshStarName(n, starBuf, sizeof(starBuf)), line,
                      (MenuOption::keyType)(i + 1), 1);
    }
  }
}

void MeshtasticApp::buildPlaces() {
  menu = newMenu("No places heard yet - COVEY's map shares them");
  uint32_t refId = meshService.getReferenceId();
  int count = meshService.getWaypointCount();
  for (int i = 0; i < count; i++) {
    const MeshWaypoint* w = meshService.getWaypoint(i);
    if (!w) {
      continue;
    }
    /* Row key = the waypoint ID, not the index: the table mutates under an open
     * menu (expiry swap-remove, LRU eviction), and a frozen index would let OK
     * set the reference to — or announce the user at — the WRONG place. An id
     * that has vanished by OK-time simply fails the lookup. (Review catch.) */
    menu->addOption(w->name,
                    (refId != 0 && w->id == refId) ? "measuring from here" : NULL,
                    (MenuOption::keyType)w->id, 1);
  }
}

void MeshtasticApp::buildPlaceOpts() {
  menu = newMenu(NULL);
  // Short on purpose: the first version's labels ran off the 240px screen
  // ("...distances from h|"). Field report, 2026-08-19.
  menu->addOption("Measure from here", MESH_KEY_SETREF, 1);
  menu->addOption("I'm here (announce)", MESH_KEY_IMHERE, 1);
}

/* The serial `sun`, on screen: dawn/sunrise/sunset/dusk at the reference place
 * plus the countdown a hunting day actually asks. Same math, same UTC ladder —
 * the almanac-verified core in sun_times.cpp does the work. Static per entry;
 * the "Refresh" soft key (OK) rebuilds, and re-entering does too. */
void MeshtasticApp::buildSun() {
  menu = newMenu(NULL);
  char line[48];

  if (!ntpClock.isTimeKnown()) {
    menu->addOption("Clock not set yet", 1, 1);
    menu->addOption("(one NTP sync on WiFi fixes it)", 2, 1);
    return;
  }
  int32_t latI = 0, lonI = 0;
  char refName[20];
  if (!meshService.resolveReference(&latI, &lonI, refName, sizeof(refName))) {
    /* The same guidance the serial command gives, in menu rows: every way a
     * reference can come to exist, including the GPS toggle one screen away. */
    menu->addOption("No place known yet", 1, 1);
    menu->addOption("Hear a waypoint (Places),", 2, 1);
    menu->addOption("pick one > I'm here, or", 3, 1);
    menu->addOption("My node > GPS receiver", 4, 1);
    return;
  }
  uint32_t utc = ntpClock.getExactUtcTime();
  int tzMin = (int)(((int64_t)ntpClock.getExactUnixTime() - (int64_t)utc) / 60);
  int y, m, d;
  sunUnixToDate(utc, &y, &m, &d);
  SunTimes t;
  if (!sunTimesUtc(y, m, d, latI * 1e-7, lonI * 1e-7, &t)) {
    menu->addOption("Computation refused", 1, 1);
    menu->addOption("(bad coordinates?)", 2, 1);
    return;
  }

  snprintf(line, sizeof(line), "At %s", refName);
  menu->addOption(line, 1, 1);

  /* Countdown FIRST — it is the question. The four times below are the paper
   * almanac for planning tomorrow. Ladder unwraps exactly as the serial does. */
  if (t.dawnMin >= 0 && t.duskMin >= 0) {
    int nowU = (int)((utc % 86400u) / 60u);
    int dawn = t.dawnMin;
    int now2 = nowU + (nowU < dawn ? 1440 : 0);
    int dusk = t.duskMin + (t.duskMin < dawn ? 1440 : 0);
    if (now2 < dawn + 1) {
      int dm = dawn - now2;
      snprintf(line, sizeof(line), "First light in %dh %02dm", dm / 60, dm % 60);
    } else if (now2 < dusk) {
      int dm = dusk - now2;
      snprintf(line, sizeof(line), "LEGAL LIGHT: %dh %02dm left", dm / 60, dm % 60);
    } else {
      strlcpy(line, "Dark now", sizeof(line));
    }
    menu->addOption(line, 2, 1);
  }

  struct SunRow { const char* label; int16_t utcMin; };
  const SunRow rows[4] = { { "First light", t.dawnMin }, { "Sunrise", t.riseMin },
                           { "Sunset", t.setMin }, { "Last light", t.duskMin } };
  for (int i = 0; i < 4; i++) {
    if (rows[i].utcMin < 0) {
      snprintf(line, sizeof(line), "%s: none today", rows[i].label);
    } else {
      int loc = ((int)rows[i].utcMin + tzMin + 2880) % 1440;
      snprintf(line, sizeof(line), "%s: %02d:%02d", rows[i].label, loc / 60, loc % 60);
    }
    menu->addOption(line, 3 + i, 1);
  }
  snprintf(line, sizeof(line), "Local clock UTC%+d:%02d", tzMin / 60, abs(tzMin % 60));
  menu->addOption(line, 7, 1);
}

void MeshtasticApp::buildStatus() {
  menu = newMenu(NULL);
  char line[40];

  const char* stateStr = "Unknown";
  switch (meshService.getRadioState()) {
  case MESH_RADIO_UNINITIALIZED: stateStr = "Uninitialized"; break;
  case MESH_RADIO_STUBBED:       stateStr = "Stubbed (no PHY)"; break;
  case MESH_RADIO_READY:         stateStr = "Ready"; break;
  case MESH_RADIO_ERROR:         stateStr = "Error"; break;
  }

  snprintf(line, sizeof(line), "Radio: %s", stateStr);
  menu->addOption(line, 1, 1);
  snprintf(line, sizeof(line), "Region: %s", meshService.getRegion());
  menu->addOption(line, 2, 1);
  snprintf(line, sizeof(line), "Channels: %d", meshService.getChannelCount());
  menu->addOption(line, 3, 1);
  snprintf(line, sizeof(line), "Preset: %s", meshService.getModemPreset());
  menu->addOption(line, 4, 1);
  snprintf(line, sizeof(line), "Node: !%08x", meshService.getMyNodeNum());
  menu->addOption(line, 5, 1);
  {
    char refName[20];
    // "From:" not "Distances from:" — the 16-char prefix left ~3 characters
    // for the place name, which is the only part carrying information.
    if (meshService.resolveReference(NULL, NULL, refName, sizeof(refName))) {
      snprintf(line, sizeof(line), "From: %s", refName);
    } else {
      snprintf(line, sizeof(line), "From: (nowhere yet)");
    }
    menu->addOption(line, 8, 1);
    /* "set (send FAILED)" is the honest state when the pin stuck locally but
     * the announce never transmitted — otherwise it reads as "they know where
     * I am" when nobody does. */
    snprintf(line, sizeof(line), "My pin: %s",
             !meshService.getMyPin(NULL, NULL, NULL) ? "not set" :
             (meshService.pinAnnounceOk() ? "set" : "set (send FAILED)"));
    menu->addOption(line, 9, 1);
  }
  snprintf(line, sizeof(line), "Nodes known: %d", meshService.getNodeCount());
  menu->addOption(line, 6, 1);
  snprintf(line, sizeof(line), "Messages: %d", meshService.getMessageCount());
  menu->addOption(line, 7, 1);
}

void MeshtasticApp::buildMyNode() {
  menu = newMenu(NULL);
  char line[48];
  menu->addOption("Edit name", MESH_MYNODE_EDIT, 1);
  menu->addOption("Edit short name", MESH_MYNODE_EDITSHORT, 1);
  menu->addOption("Request node info", MESH_MYNODE_REQUEST, 1);
  snprintf(line, sizeof(line), "Hop limit: %u (tap to change)", meshService.getHopLimit());
  menu->addOption(line, MESH_MYNODE_HOPLIMIT, 1);
#ifdef USER_SERIAL
  {
    /* The woods plate's GPS. "on (fix)" only while the fix is FRESH — a stale
     * fix labelled "fix" would read as "working" with the antenna unplugged. */
    const char* gs = "off";
    if (gGpsNmea) {
      uint32_t age = 0;
      gs = (meshService.getGpsFix(NULL, NULL, &age, NULL, NULL) && age < 120000UL)
           ? "on (fix)" : "on (no fix yet)";
    }
    snprintf(line, sizeof(line), "GPS receiver: %s", gs);
    menu->addOption(line, MESH_MYNODE_GPS, 1);
  }
#endif
  /* Neighbour announce (portnum 71): who we hear DIRECTLY, told to the private
   * channels so a who-hears-whom map can be built. Names the channel it will
   * use, because "on" with no private channel to send on would be a lie. */
  {
    const uint32_t iv = meshService.getNeighborInterval();
    const char* chn = meshService.neighborChannelName();
    if (iv == 0) {
      snprintf(line, sizeof(line), "Neighbor info: off");
    } else if (!chn) {
      snprintf(line, sizeof(line), "Neighbor info: %luh - NO PRIVATE CHANNEL",
               (unsigned long)(iv / 3600));
    } else {
      snprintf(line, sizeof(line), "Neighbor info: every %luh (%d heard)",
               (unsigned long)(iv / 3600), meshService.getDirectNeighborCount());
    }
    menu->addOption(line, MESH_MYNODE_NEIGHBOR, 1);
  }
  snprintf(line, sizeof(line), "Name: %s", meshService.getMyLongName());
  menu->addOption(line, 3, 1);
  /* Show whether the short name is the user's or follows the long name — otherwise there is
   * no way to tell why it changed by itself after a rename. */
  snprintf(line, sizeof(line), "Short: %s%s", meshService.getMyShortName(),
           meshService.isShortNameCustom() ? "" : " (auto)");
  menu->addOption(line, 9, 1);
  snprintf(line, sizeof(line), "Node: !%08x", meshService.getMyNodeNum());
  menu->addOption(line, 4, 1);
  // Destructive: require a second press to confirm.
  menu->addOption(pendingClear == 1 ? "Clear messages? (confirm)" : "Clear messages",
                  MESH_MYNODE_CLEARMSGS, 1);
  menu->addOption(pendingClear == 2 ? "Clear nodes? (confirm)" : "Clear nodes",
                  MESH_MYNODE_CLEARNODES, 1);
}

void MeshtasticApp::buildEditName() {
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "Node name", controlState, MESH_NAME_LEN - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);  // white text on black (theme)
  textArea->setText(meshService.getMyLongName());
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

/* The 4-character name other radios put in their node lists. Kept separate from the long
 * name because Meshtastic sends both and clients lay out for four characters. Clearing the
 * field and saving puts it back to following the long name. */
void MeshtasticApp::buildEditShortName() {
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "4 chars, blank = auto", controlState, MESH_SHORT_NAME_MAX,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);
  /* Only prefill a name the user chose. Prefilling a derived one would silently turn it into
   * a custom one the moment they pressed Save, and then it would stop following the long
   * name without them ever asking for that. */
  textArea->setText(meshService.isShortNameCustom() ? meshService.getMyShortName() : "");
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

void MeshtasticApp::buildCompose() {
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     threadIsChannel ? "Type a message" : "Type a direct message",
                                     controlState, MESH_TEXT_LEN - 1,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);  // white text on black (theme)
  textArea->setFocus(true);
  controlState.setInputState(InputType::AlphaNum);
}

void MeshtasticApp::buildViewMessage(int msgIndex) {
  viewMsgIndex = msgIndex;
  const int16_t padding = 4;
  textArea = new MultilineTextWidget(0, header->height(), lcd.width(),
                                     lcd.height() - header->height() - footer->height(),
                                     "Empty message", controlState, 10000,
                                     fonts[OPENSANS_COND_BOLD_20], InputType::AlphaNum, padding, padding);
  textArea->setColors(WP_COLOR_1, WP_COLOR_0);  // white text on black (theme)

  const MeshMessage* m = meshService.getMessage(msgIndex);
  if (m) {
    char who[MESH_NAME_LEN];
    if (m->flags & MESH_MSG_OUTGOING) {
      strlcpy(who, "me", sizeof(who));
    } else {
      meshNodeLabel(m->from, who, sizeof(who));
    }
    char full[MESH_TEXT_LEN + MESH_NAME_LEN + 8];
    snprintf(full, sizeof(full), "%s:\n%s", who, m->text);
    textArea->setText(full);
  }
  textArea->cursorToStart();
  textArea->setFocus(true);
}

appEventResult MeshtasticApp::processEvent(EventType event) {
  /* A new inbound message OR a position/waypoint arrival: keep list views live.
   * Without the Places/Nodes cases this reproduced the book-sync trap — the
   * user sits on "No places heard yet" while camp has already arrived. Rebuild
   * resets the menu selection to the top; the rows are id-keyed so at worst the
   * highlight jumps, never the action target. */
  if (event == NEW_MESSAGE_EVENT) {
    if (appState == MESH_THREAD)      { buildThread();  return REDRAW_SCREEN; }
    else if (appState == MESH_CHATS)  { buildChats();   return REDRAW_SCREEN; }
    else if (appState == MESH_MAIN)   { buildMainMenu(); return REDRAW_SCREEN; }
    else if (appState == MESH_NODES)  { buildNodes();   return REDRAW_SCREEN; }
    else if (appState == MESH_PLACES) { buildPlaces();  return REDRAW_SCREEN; }
    else if (appState == MESH_STATUS) { buildStatus();  return REDRAW_SCREEN; }
    else if (appState == MESH_PLACE_OPTS &&
             !meshService.findWaypoint(selectedWaypointId)) {
      enterState(MESH_PLACES);        // the place vanished under us: back out
      return REDRAW_ALL;
    }
    return DO_NOTHING;
  }

  switch (appState) {

  case MESH_MAIN:
    if (LOGIC_BUTTON_BACK(event)) {
      return EXIT_APP;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      switch (menu->currentKey()) {
      case MESH_KEY_CHATS:  enterState(MESH_CHATS);  break;
      case MESH_KEY_NODES:  enterState(MESH_NODES);  break;
      case MESH_KEY_PLACES: enterState(MESH_PLACES); break;
      case MESH_KEY_STATUS: enterState(MESH_STATUS); break;
      case MESH_KEY_MYNODE: enterState(MESH_MYNODE); break;
      case MESH_KEY_SUN:    enterState(MESH_SUN);    break;
      default: return REDRAW_SCREEN;
      }
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;

  case MESH_PLACES:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      uint32_t id = (uint32_t)menu->currentKey();       // rows are keyed by waypoint id
      if (meshService.findWaypoint(id)) {
        selectedWaypointId = id;
        enterState(MESH_PLACE_OPTS);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MESH_PLACE_OPTS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_PLACES);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      const MeshWaypoint* w = meshService.findWaypoint(selectedWaypointId);
      if (w && menu->currentKey() == MESH_KEY_SETREF) {
        meshService.setReferenceId(w->id);
        /* Jump straight to WHERE THE ANSWER IS. The first version returned to
         * Places with a subtle [ref] tag, and the field report was "nothing
         * happened anywhere?" — the distances were sitting in Nodes, unvisited.
         * Show the payoff, don't make the user hunt for it. */
        enterState(MESH_NODES);
        return REDRAW_ALL;
      } else if (w && menu->currentKey() == MESH_KEY_IMHERE) {
        /* Declare the phone AT this waypoint: persists, and broadcasts one
         * Position so COVEY's map shows this phone. The declaration is a
         * statement by the user, not a fix — there is no GPS to argue. */
        meshService.setMyPin(w->latI, w->lonI);
      }
      enterState(MESH_PLACES);            // the [ref] marker redraws
      return REDRAW_ALL;
    }
    return REDRAW_SCREEN;

  case MESH_CHATS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      int idx = (int)menu->currentKey() - 1;
      if (idx >= 0 && idx < chatCount) {
        if (chatIsChannel[idx]) {
          threadIsChannel = true;
          threadChannelHash = (uint8_t)chatId[idx];
        } else {
          threadIsChannel = false;
          threadPeer = chatId[idx];
        }
        enterState(MESH_THREAD);
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MESH_THREAD:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_CHATS);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == MESH_KEY_WRITE) {
        returnState = MESH_THREAD;
        enterState(MESH_COMPOSE);
        return REDRAW_ALL;
      } else if (sel >= 1) {
        returnState = MESH_THREAD;
        buildViewMessage((int)sel - 1);
        appState = MESH_VIEWMSG;
        header->setTitle("Message");
        footer->setButtons("Apply link", "Back");
        return REDRAW_ALL;
      }
    }
    return REDRAW_SCREEN;

  case MESH_NODES:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    /* ⭐ The STAR KEY stars the node. OK is already "DM this node" and the list has no spare
     * soft key, so this needed a plain key — and `*` naming the thing it does is the one
     * mapping nobody has to be told twice. Starred nodes sort to the top of this list AND
     * become the last thing evicted when the table fills, which on a public LongFast mesh is
     * the half that actually matters. */
    if (event == '*') {
      MenuOption::keyType star = menu->currentKey();
      if (star >= 1) {
        const MeshNode* sn = meshService.getNode((int)star - 1);
        if (sn && meshService.toggleFavourite(sn->nodeNum)) { /* starred */ }
        buildNodes();                          // re-sorts: it may have just jumped to the top
        return REDRAW_SCREEN;
      }
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel >= 1) {
        const MeshNode* n = meshService.getNode((int)sel - 1);
        if (n && n->nodeNum != meshService.getMyNodeNum()) {
          threadIsChannel = false;             // compose a DM to this node
          threadPeer = n->nodeNum;
          returnState = MESH_NODES;
          enterState(MESH_COMPOSE);
          return REDRAW_ALL;
        }
      }
    }
    return REDRAW_SCREEN;

  case MESH_STATUS:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    return REDRAW_SCREEN;

  case MESH_SUN:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {      // "Refresh": recompute the countdown
      enterState(MESH_SUN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    return REDRAW_SCREEN;

  case MESH_MYNODE:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
      if (sel == MESH_MYNODE_EDITSHORT) {
        enterState(MESH_EDITSHORT);
        return REDRAW_ALL;
      }
      if (sel == MESH_MYNODE_EDIT) {
        enterState(MESH_EDITNAME);
        return REDRAW_ALL;
      } else if (sel == MESH_MYNODE_REQUEST) {
        pendingClear = 0;
        meshService.announceNodeInfo(true);
        buildMyNode();
        menu->select(MESH_MYNODE_REQUEST);
      } else if (sel == MESH_MYNODE_HOPLIMIT) {
        pendingClear = 0;
        uint8_t h = meshService.getHopLimit() + 1;   // cycle 1..7
        if (h > 7) h = 1;
        meshService.setHopLimit(h);
        buildMyNode();
        menu->select(MESH_MYNODE_HOPLIMIT);
      } else if (sel == MESH_MYNODE_CLEARMSGS) {
        if (pendingClear == 1) { meshService.clearMessages(); pendingClear = 0; }
        else { pendingClear = 1; }
        buildMyNode();
        menu->select(MESH_MYNODE_CLEARMSGS);
      } else if (sel == MESH_MYNODE_CLEARNODES) {
        if (pendingClear == 2) { meshService.clearNodes(); pendingClear = 0; }
        else { pendingClear = 2; }
        buildMyNode();
        menu->select(MESH_MYNODE_CLEARNODES);
#ifdef USER_SERIAL
      } else if (sel == MESH_MYNODE_GPS) {
        pendingClear = 0;
        gGpsNmea = !gGpsNmea;         // same flag + pref as serial `gps on|off`
        {
          Preferences p;
          p.begin("wpmesh", false);
          p.putBool("gpsen", gGpsNmea);
          p.end();
        }
        gpsApplyBaud(gGpsNmea);       // ...and the same UART retune, or the GUI
                                      // toggle would leave the port at the other
                                      // consumer's rate and read nothing
        buildMyNode();
        menu->select(MESH_MYNODE_GPS);
#endif
      } else if (sel == MESH_MYNODE_NEIGHBOR) {
        pendingClear = 0;
        /* off -> 1h -> 4h -> off. One hour is the useful one for a hunting day
         * (a four-hour cadence gives about three updates between dawn and
         * dusk); four matches what the stock module's own docs call its floor,
         * for anyone who would rather be quieter on the band. */
        const uint32_t iv = meshService.getNeighborInterval();
        meshService.setNeighborInterval(iv == 0 ? 3600 : (iv == 3600 ? 14400 : 0));
        buildMyNode();
        menu->select(MESH_MYNODE_NEIGHBOR);
      } else {
        pendingClear = 0;
      }
    }
    return REDRAW_SCREEN;

  case MESH_EDITNAME:
    if (event == WIPHONE_KEY_END) {            // cancel (Back is backspace)
      enterState(MESH_MYNODE);
      return REDRAW_ALL;
    }
    // Save is the "Save" soft key, not the D-pad centre — the centre is committing multi-tap
    // letters while you type here, exactly as on the compose screen.
    if (LOGIC_BUTTON_SEND(event)) {            // save
      const char* name = textArea ? textArea->getText() : NULL;
      if (name && name[0]) {
        meshService.setMyName(name);
      }
      enterState(MESH_MYNODE);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_OK) {
      return DO_NOTHING;                       // nothing left to commit; do not save either
    }
    if (IS_KEYBOARD(event) && textArea) {
      textArea->processEvent(event);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case MESH_EDITSHORT:
    if (event == WIPHONE_KEY_END) {            // cancel (Back is backspace)
      enterState(MESH_MYNODE);
      return REDRAW_ALL;
    }
    // Save is the "Save" soft key, not the D-pad centre — see MESH_EDITNAME above.
    if (LOGIC_BUTTON_SEND(event)) {            // save
      /* ⚠ An EMPTY field is meaningful here and must be passed through, unlike Edit name
       * which ignores it: it means "go back to deriving the short name from the long one".
       * Rejecting it would leave no way to undo a custom short name. */
      meshService.setMyShortName(textArea ? textArea->getText() : NULL);
      enterState(MESH_MYNODE);
      return REDRAW_ALL;
    }
    if (event == WIPHONE_KEY_OK) {
      return DO_NOTHING;                       // nothing left to commit; do not save either
    }
    if (IS_KEYBOARD(event) && textArea) {
      textArea->processEvent(event);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case MESH_COMPOSE:
    if (event == WIPHONE_KEY_END) {        // cancel (Back is backspace in the input)
      enterState(returnState);
      return REDRAW_ALL;
    }
    /* ⚠ SEND, not OK — the D-pad centre must never send from here.
     * On this screen the centre is a TYPING key: it commits the highlighted multi-tap
     * letter. It was also an OK, so the press after your last letter posted the message
     * half-written. Send is the top-left soft key, or CALL. */
    if (LOGIC_BUTTON_SEND(event)) {
      const char* msg = textArea ? textArea->getText() : NULL;
      if (msg && msg[0]) {
        if (threadIsChannel) {
          meshService.sendChannelMessage(threadChannelHash, msg);
        } else {
          meshService.sendDirectMessage(threadPeer, msg);
        }
        enterState(MESH_THREAD);           // drop into the conversation we posted to
        return REDRAW_ALL;
      }
      return DO_NOTHING;                    // nothing typed yet; stay in Compose
    }
    if (event == WIPHONE_KEY_OK) {
      /* Swallow it. A centre press reaching here had no pending letter left to commit, so
       * there is nothing to do — and it must not fall through to anything else. */
      return DO_NOTHING;
    }
    if (IS_KEYBOARD(event) && textArea) {
      textArea->processEvent(event);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;

  case MESH_VIEWMSG:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(returnState);
      return REDRAW_ALL;
    }
    if (LOGIC_BUTTON_OK(event)) {           // "Apply link": try to import channels
      const MeshMessage* m = (viewMsgIndex >= 0) ? meshService.getMessage(viewMsgIndex) : NULL;
      int added = m ? meshService.applyChannelUrl(m->text) : 0;
      char res[64];
      if (added > 0) {
        snprintf(res, sizeof(res), "Applied: %d channel(s) added.", added);
      } else {
        snprintf(res, sizeof(res), "No channel link found in this message.");
      }
      if (textArea) {
        textArea->setText(res);
        textArea->cursorToStart();
      }
      return REDRAW_SCREEN;
    }
    if ((event == WIPHONE_KEY_UP || event == WIPHONE_KEY_DOWN) && textArea) {
      textArea->processEvent(event);
      return REDRAW_SCREEN;
    }
    return DO_NOTHING;
  }

  return DO_NOTHING;
}

void MeshtasticApp::redrawScreen(bool redrawAll) {
  if (appState == MESH_COMPOSE || appState == MESH_VIEWMSG || appState == MESH_EDITNAME ||
      appState == MESH_EDITSHORT) {
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
  } else if (menu) {
    ((GUIWidget*)menu)->redraw(lcd);
  }
}
