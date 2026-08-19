#include "sms_mirror_rx.h"
#include "sms_mirror.h"
#include "GUI.h"
#include "config.h"
#include "clock.h"     // ntpClock: the buzz is gated on the record being RECENT

#include <string.h>
#include <stdio.h>

extern GUI gui;

/* Latched by smsMirrorIngestLine, taken by the main loop — see the note in the header. */
static bool s_newStored  = false;
static bool s_newInbound = false;

bool smsMirrorTakeNews(bool* inbound) {
  const bool stored = s_newStored;
  if (inbound) {
    *inbound = s_newInbound;
  }
  s_newStored = false;
  s_newInbound = false;
  return stored;
}

bool smsMirrorIsMirrorLine(const char* text) {
  return smsMirrorIsMirrorText(text);
}

/* Build the correspondent's URI in the form THIS PHONE would build it.
 *
 * ⚠ This matters more than it looks. `TinySIP::sendMessage()` uses the address VERBATIM —
 * unlike calling, which auto-appends the server for a bare number — so a stored correspondent
 * of "4257604281" would call fine and silently fail to text. That trap is already written up
 * in the handoff, and storing a bare number here would walk straight back into it.
 *
 * The host comes from the phone's own account rather than from anything COVEY sent, because
 * COVEY reaches VoIP.ms over a REST API and has no SIP host to offer. `565611_nikguy@
 * seattle1.voip.ms` therefore yields `14257604281@seattle1.voip.ms`, which is exactly the
 * phonebook form that is known to work for both calling and texting.
 */
static bool buildPeerUri(const char* digits, const char* ownUri, char* out, size_t cap) {
  const char* at = ownUri ? strchr(ownUri, '@') : NULL;
  if (!at || !at[1] || !digits || !digits[0]) {
    return false;
  }
  // A 10-digit NANP number is dialled with a leading 1 on this account; anything else is
  // passed through as-is rather than guessed at.
  int w = (strlen(digits) == 10)
          ? snprintf(out, cap, "1%s%s", digits, at)
          : snprintf(out, cap, "%s%s", digits, at);
  return w > 0 && (size_t)w < cap;
}

int smsMirrorIngestLine(const char* line, bool* wasIncoming) {
  if (wasIncoming) {
    *wasIncoming = false;
  }
  SmsMirrorRecord rec;
  if (!smsMirrorUnpack(line, &rec)) {
    return -1;
  }

  const char* ownUri = gui.state.fromUriDyn;
  if (!ownUri || !*ownUri) {
    /* No SIP account: refuse rather than invent a host. Storing under a made-up URI would
     * put the thread somewhere a reply could never reach, and it would not merge with the
     * real thread once an account was configured. */
    log_e("SMS mirror: no SIP account, refusing record %lld", (long long)rec.id);
    return -1;
  }

  char peerUri[128];
  if (!buildPeerUri(rec.peer, ownUri, peerUri, sizeof(peerUri))) {
    log_e("SMS mirror: cannot build a peer URI from '%s'", rec.peer);
    return -1;
  }

  if (!gui.flash.messages.isLoaded()) {
    gui.flash.messages.load(ntpClock.isTimeKnown() ? ntpClock.getExactUnixTime() : 0);
  }

  int r = gui.flash.messages.ingestMirrored(rec, ownUri, peerUri);

  /* ⚠ The preloaded cache has been disturbed by the scan inside ingestMirrored(). Clearing
   * it here rather than leaving it to the caller keeps the two transports from having to
   * remember: a Messages screen that is open rebuilds its menu from a clean cache on its
   * next NEW_MESSAGE_EVENT, which is the same path a delete already uses. */
  gui.flash.messages.clearPreloaded();

  /* Raise the SIP unread badge here, because the transports deliberately do NOT raise their
   * own. `unreadMessages` is event-driven, not polled — it is refreshed by reloadMessages()
   * and after reading a message, and nowhere else — so without this line an incoming
   * mirrored text would sit in the store with nothing on screen to say it had arrived.
   *
   * ⚠ Only INCOMING messages carry the unread flag ("u" is set by saveMessage only when
   * incoming), so a text mirrored back from something Nick sent on COVEY lights nothing.
   * That is the intended behaviour: he already knows about it. */
  if (r > 0) {
    gui.state.unreadMessages = gui.flash.messages.hasUnread();
    if (wasIncoming) {
      *wasIncoming = !rec.out;      // `out` is COVEY's view: the ACCOUNT sent it
    }
    /* Latch for the main loop's announcer — whichever transport this line rode in on.
     *
     * ⚠ The BUZZ latch additionally requires the record to be RECENT. A full resync
     * (first boot with an empty store, or a deleted since-file) stores hours or days of
     * old inbound texts in one poll, and each would otherwise latch a buzz — the motor
     * running continuously for the length of the catch-up, announcing history. Ten
     * minutes of slack covers every real transport delay (LoRa store-and-forward, a poll
     * gap, COVEY catching up after ITS fix) without letting an archive vibrate. When the
     * clock is not yet known the buzz wins over silence: a wrongly silent real arrival
     * is the exact fault this feature exists to fix. The UI latch is unconditional —
     * the screen should refresh whatever the age. */
    s_newStored = true;
    if (!rec.out) {
      const bool recent = !ntpClock.isTimeKnown() || rec.ts == 0 ||
                          (int64_t)ntpClock.getExactUnixTime() - (int64_t)rec.ts < 600;
      if (recent) {
        s_newInbound = true;
      }
    }
  }

  return r;
}
