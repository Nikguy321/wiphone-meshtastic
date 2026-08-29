/*
 * booksync_inbox.cpp — see booksync_inbox.h.
 */
#include "booksync_inbox.h"

#include <string.h>

static BookSyncInboxItem s_items[BOOKSYNC_INBOX_MAX];
static int s_count = 0;
static uint32_t s_seq = 0;

void bookSyncInboxInit() {
  s_count = 0;
  memset(s_items, 0, sizeof(s_items));
  s_seq++;                             // never reset: a reader holding an old value must not match
}

uint32_t bookSyncInboxSeq() {
  return s_seq;
}

bool bookSyncInboxPush(const char* text, uint32_t from, uint32_t rxUnix) {
  if (!text || !bookSyncIsSyncText(text)) {
    return false;
  }
  size_t len = strlen(text);
  if (len >= BOOKSYNC_MESH_TEXT_MAX) {
    return false;                    // longer than this protocol can produce: not ours
  }

  // A resend of the same packet is the same news. Replace it in place rather than filling the
  // inbox with copies, which would push out positions for OTHER books.
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_items[i].text, text) == 0) {
      s_items[i].from = from;
      s_items[i].rxUnix = rxUnix;
      return true;                     // deliberately no s_seq bump — see the note in the header
    }
  }

  if (s_count >= BOOKSYNC_INBOX_MAX) {
    for (int i = 0; i + 1 < BOOKSYNC_INBOX_MAX; i++) {
      s_items[i] = s_items[i + 1];   // drop the oldest
    }
    s_count = BOOKSYNC_INBOX_MAX - 1;
  }
  BookSyncInboxItem* it = &s_items[s_count++];
  memset(it, 0, sizeof(*it));
  memcpy(it->text, text, len);
  it->text[len] = '\0';
  it->from = from;
  it->rxUnix = rxUnix;
  s_seq++;
  return true;
}

int bookSyncInboxCount() {
  return s_count;
}

const BookSyncInboxItem* bookSyncInboxGet(int i) {
  return (i >= 0 && i < s_count) ? &s_items[i] : NULL;
}

void bookSyncInboxRemove(int i) {
  if (i < 0 || i >= s_count) {
    return;
  }
  for (int k = i; k + 1 < s_count; k++) {
    s_items[k] = s_items[k + 1];
  }
  s_count--;
  /* Bumped on the way out as well as in, so that acting on one offer makes the reader look
   * again: two devices can each park a position for the same book, and the second must not
   * stay invisible until the next time the book is opened. */
  s_seq++;
}

int bookSyncInboxFindFor(const uint8_t key[32], const char* const* ids, int nIds,
                         BookSyncRecord* out, uint32_t* fromOut) {
  int best = -1;
  BookSyncRecord bestRec;
  memset(&bestRec, 0, sizeof(bestRec));

  for (int i = 0; i < s_count; i++) {
    BookSyncRecord r;
    // Verifies the mac as well as parsing: a packet signed with a different passcode simply
    // never matches, which is the whole reason the radio path did not need the key.
    if (!bookSyncUnpackMesh(s_items[i].text, key, &r)) {
      continue;
    }
    if (!bookSyncRecordMatchesIds(&r, ids, nIds)) {
      continue;
    }
    // Newest by the sender's reading event, not by arrival: a packet delayed by the mesh
    // still describes when the page was turned.
    if (best < 0 || r.turnedAt > bestRec.turnedAt) {
      best = i;
      bestRec = r;
    }
  }

  if (best >= 0) {
    if (out) {
      *out = bestRec;
    }
    if (fromOut) {
      *fromOut = s_items[best].from;
    }
  }
  return best;
}
