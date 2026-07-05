/*
 * app_meshtastic.cpp — WiPhone Meshtastic UI (multi-channel conversations)
 *
 * Chats lists one thread per known channel (LongFast + any imported custom
 * channels) plus one thread per DM peer. Viewing a message that contains a
 * Meshtastic channel link lets you apply it ("Apply link" = OK in the viewer).
 */

#include "app_meshtastic.h"
#include "meshtastic_service.h"

// Main-menu option keys
#define MESH_KEY_CHATS      1
#define MESH_KEY_NODES      2
#define MESH_KEY_STATUS     3
#define MESH_KEY_MYNODE     4

// "My node" screen option keys
#define MESH_MYNODE_EDIT       1
#define MESH_MYNODE_REQUEST    2
#define MESH_MYNODE_HOPLIMIT   7
#define MESH_MYNODE_CLEARMSGS  5
#define MESH_MYNODE_CLEARNODES 6

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
      header->setTitle(ch ? ch->name : "Channel");
    } else {
      char t[MESH_NAME_LEN];
      meshNodeLabel(threadPeer, t, sizeof(t));
      header->setTitle(t);
    }
    footer->setButtons("Select", "Back");
    buildThread();
    break;
  }
  case MESH_NODES:
    header->setTitle("Nodes");
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
    footer->setButtons("Send", "Cancel");
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
  case MESH_EDITNAME:
    header->setTitle("Edit name");
    footer->setButtons("Save", "Cancel");
    buildEditName();
    break;
  }
}

void MeshtasticApp::buildMainMenu() {
  menu = newMenu(NULL);
  char line[24];
  menu->addOption("Chats", MESH_KEY_CHATS, 1);
  snprintf(line, sizeof(line), "Nodes (%d)", meshService.getNodeCount());
  menu->addOption(line, MESH_KEY_NODES, 1);
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
    snprintf(line, sizeof(line), "%s (%d)", name, counts[j]);
    menu->addOption(line, (MenuOption::keyType)(j + 1), 1);
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
    if (m->flags & MESH_MSG_OUTGOING) {
      who = "me";
    } else {
      meshNodeLabel(m->from, whoBuf, sizeof(whoBuf));
      who = whoBuf;
    }
    snprintf(line, sizeof(line), "%s: %s", who, m->text);
    menu->addOption(line, (MenuOption::keyType)(i + 1), 1);   // key = global msg index + 1
  }
}

void MeshtasticApp::buildNodes() {
  menu = newMenu("No nodes heard yet");
  char line[64];
  int count = meshService.getNodeCount();
  for (int i = 0; i < count; i++) {
    const MeshNode* n = meshService.getNode(i);
    if (!n) {
      continue;
    }
    snprintf(line, sizeof(line), "%s  (!%08x)", n->name, n->nodeNum);
    menu->addOption(line, (MenuOption::keyType)(i + 1), 1);
  }
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
  snprintf(line, sizeof(line), "Nodes known: %d", meshService.getNodeCount());
  menu->addOption(line, 6, 1);
  snprintf(line, sizeof(line), "Messages: %d", meshService.getMessageCount());
  menu->addOption(line, 7, 1);
}

void MeshtasticApp::buildMyNode() {
  menu = newMenu(NULL);
  char line[48];
  menu->addOption("Edit name", MESH_MYNODE_EDIT, 1);
  menu->addOption("Request node info", MESH_MYNODE_REQUEST, 1);
  snprintf(line, sizeof(line), "Hop limit: %u (tap to change)", meshService.getHopLimit());
  menu->addOption(line, MESH_MYNODE_HOPLIMIT, 1);
  snprintf(line, sizeof(line), "Name: %s", meshService.getMyLongName());
  menu->addOption(line, 3, 1);
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
  // A new inbound message arrived: keep list views live.
  if (event == NEW_MESSAGE_EVENT) {
    if (appState == MESH_THREAD)      { buildThread();  return REDRAW_SCREEN; }
    else if (appState == MESH_CHATS)  { buildChats();   return REDRAW_SCREEN; }
    else if (appState == MESH_MAIN)   { buildMainMenu(); return REDRAW_SCREEN; }
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
      case MESH_KEY_STATUS: enterState(MESH_STATUS); break;
      case MESH_KEY_MYNODE: enterState(MESH_MYNODE); break;
      default: return REDRAW_SCREEN;
      }
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

  case MESH_MYNODE:
    if (LOGIC_BUTTON_BACK(event)) {
      enterState(MESH_MAIN);
      return REDRAW_ALL;
    }
    menu->processEvent(event);
    if (LOGIC_BUTTON_OK(event)) {
      MenuOption::keyType sel = menu->currentKey();
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
    if (LOGIC_BUTTON_OK(event)) {              // save
      const char* name = textArea ? textArea->getText() : NULL;
      if (name && name[0]) {
        meshService.setMyName(name);
      }
      enterState(MESH_MYNODE);
      return REDRAW_ALL;
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
    if (LOGIC_BUTTON_OK(event)) {          // send
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
  if (appState == MESH_COMPOSE || appState == MESH_VIEWMSG || appState == MESH_EDITNAME) {
    if (textArea) {
      ((GUIWidget*)textArea)->redraw(lcd);
    }
  } else if (menu) {
    ((GUIWidget*)menu)->redraw(lcd);
  }
}
