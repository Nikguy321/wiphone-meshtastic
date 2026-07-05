/*
 * app_meshtastic.h — WiPhone Meshtastic UI (Phase 4b: conversations)
 *
 * Main menu -> Chats / Nodes / Status. "Chats" lists conversations: a pinned
 * "Main Channel" (broadcast) plus one thread per DM peer, most-recent first.
 * Selecting a chat opens that thread; a thread offers "Write" (compose targeting
 * the thread) and opening any message for a full, word-wrapped, scrollable view.
 */

#ifndef APP_MESHTASTIC_H
#define APP_MESHTASTIC_H

#include "GUI.h"

// Max distinct DM conversations we can list (bounded by the message ring size).
#define MESH_APP_MAX_CHATS  24

class MeshtasticApp : public WindowedApp {
public:
  MeshtasticApp(LCD& disp, ControlState& state, HeaderWidget* header, FooterWidget* footer);
  virtual ~MeshtasticApp();

  ActionID_t getId() {
    return GUI_APP_MESHTASTIC;
  };
  appEventResult processEvent(EventType event);
  void redrawScreen(bool redrawAll=false);

protected:
  typedef enum {
    MESH_MAIN,
    MESH_CHATS,
    MESH_THREAD,
    MESH_NODES,
    MESH_STATUS,
    MESH_COMPOSE,
    MESH_VIEWMSG,
    MESH_MYNODE,
    MESH_EDITNAME,
  } MeshAppState_t;

  MeshAppState_t appState;
  MeshAppState_t returnState;       // where Compose/View return to on Back/Cancel

  MenuWidget*          menu;        // active list widget
  MultilineTextWidget* textArea;    // active text widget (Compose/View)

  // Current thread / compose target.
  bool     threadIsChannel;         // true = channel thread, false = DM thread
  uint8_t  threadChannelHash;       // channel thread's channel hash
  uint32_t threadPeer;              // DM thread's peer node

  int      viewMsgIndex;            // message index shown in the View screen
  int      pendingClear;            // My node: 0 none, 1 messages, 2 nodes (confirm)

  // Chats-list entries: a channel (isChannel true, id = channel hash) or a DM
  // peer (isChannel false, id = peer node number). Row key = index + 1.
  bool     chatIsChannel[MESH_APP_MAX_CHATS];
  uint32_t chatId[MESH_APP_MAX_CHATS];
  int      chatCount;

  void freeWidgets();
  void enterState(MeshAppState_t state);

  MenuWidget* newMenu(const char* emptyMessage);
  void buildMainMenu();
  void buildChats();
  void buildThread();
  void buildNodes();
  void buildStatus();
  void buildCompose();
  void buildViewMessage(int msgIndex);
  void buildMyNode();
  void buildEditName();
};

#endif // APP_MESHTASTIC_H
