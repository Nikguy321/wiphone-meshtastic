/* Host-side test for the T9 engine. Builds and runs on the laptop, not the phone:
 *
 *     c++ -std=c++11 -I WiPhone -o /tmp/t9test tools/t9_hosttest.cpp WiPhone/t9.cpp && /tmp/t9test
 *
 * t9.cpp deliberately depends on nothing but <stdint.h> and the generated table, which is
 * what makes this possible — and it is the reason the engine was kept free of widgets,
 * ControlState and the keypad. A logic bug found here costs a second; the same bug found
 * after flashing costs a reflash and a reboot.
 *
 * These are behavioural assertions, not a demo: every one of them is a property the input
 * path will rely on.
 */
#include "t9.h"
#include "src/assets/t9_dict.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) {
    failures++;
    std::printf("  FAIL  %s\n", what);
  }
}

static void eqs(const char* got, const char* want, const char* what) {
  checks++;
  const bool same = (got == NULL && want == NULL) ||
                    (got != NULL && want != NULL && std::strcmp(got, want) == 0);
  if (!same) {
    failures++;
    std::printf("  FAIL  %s: got %s, want %s\n", what,
                got ? got : "(null)", want ? want : "(null)");
  }
}

// Type a whole digit string into a fresh engine.
static void type(T9Engine& e, const char* digits) {
  e.clear();
  for (const char* p = digits; *p; p++) {
    e.pushDigit(*p);
  }
}

// The digit key of a word, computed independently of the engine so the test is not just
// asserting that t9.cpp agrees with itself.
static void keyOf(const char* w, char* out) {
  int n = 0;
  for (; *w; w++) {
    const char c = *w | 0x20;
    if (c < 'a' || c > 'z') {
      continue;                       // apostrophe
    }
    static const char* rows[8] = { "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
    for (int r = 0; r < 8; r++) {
      if (std::strchr(rows[r], c)) {
        out[n++] = (char)('2' + r);
        break;
      }
    }
  }
  out[n] = '\0';
}

int main() {
  std::printf("T9 engine host test — %d words in table\n", (int)T9_WORD_COUNT);

  // ---- the table's own invariants ------------------------------------------------------
  // Everything below depends on the table being sorted the way the generator claims. Check
  // it here rather than trusting it: a mis-sorted table would make the binary search return
  // plausible-looking wrong answers rather than failing loudly.
  {
    char prev[64] = "", cur[64];
    bool sorted = true;
    int maxLen = 0, maxKey = 0;
    for (int i = 0; i < (int)T9_WORD_COUNT; i++) {
      keyOf(T9_WORDS[i], cur);
      if (std::strcmp(prev, cur) > 0) {
        sorted = false;
        std::printf("  ...out of order at %d: %s (%s) after (%s)\n", i, T9_WORDS[i], cur, prev);
        break;
      }
      const int l = (int)std::strlen(T9_WORDS[i]);
      if (l > maxLen) maxLen = l;
      if ((int)std::strlen(cur) > maxKey) maxKey = (int)std::strlen(cur);
      std::strcpy(prev, cur);
    }
    ok(sorted, "table is sorted by digit key");
    ok(maxLen <= T9_MAX_WORD_LEN, "no word exceeds T9_MAX_WORD_LEN");
    ok(maxKey <= T9_MAX_KEY_LEN, "no key exceeds T9_MAX_KEY_LEN");
  }

  // ---- the digit mapping ---------------------------------------------------------------
  ok(t9DigitForChar('a') == '2' && t9DigitForChar('c') == '2', "abc -> 2");
  ok(t9DigitForChar('s') == '7' && t9DigitForChar('p') == '7', "pqrs -> 7");
  ok(t9DigitForChar('z') == '9', "z -> 9");
  ok(t9DigitForChar('A') == '2', "uppercase maps too");
  ok(t9DigitForChar('\'') == 0, "apostrophe has no digit");
  ok(t9DigitForChar('0') == 0 && t9DigitForChar(' ') == 0, "non-letters have no digit");
  ok(!t9IsWordDigit('0') && !t9IsWordDigit('1'), "0 and 1 are not word digits");
  ok(t9IsWordDigit('2') && t9IsWordDigit('9'), "2..9 are word digits");

  // ---- empty state ---------------------------------------------------------------------
  {
    T9Engine e;
    ok(!e.pending(), "starts with nothing pending");
    ok(e.candidateCount() == 0, "no candidates when empty");
    eqs(e.current(), NULL, "current() is NULL when empty");
    ok(e.candidate(0) == NULL && e.candidate(-1) == NULL, "candidate() bounds-checks");
    ok(!e.popDigit(), "popDigit on empty returns false");
    e.nextCandidate();                       // must not crash or misbehave
    e.prevCandidate();
    ok(e.candidateCount() == 0, "cycling with no candidates is a no-op");
  }

  // ---- rejects what is not a word digit -------------------------------------------------
  {
    T9Engine e;
    ok(!e.pushDigit('0'), "0 rejected");
    ok(!e.pushDigit('1'), "1 rejected");
    ok(!e.pushDigit('a'), "letter rejected");
    ok(!e.pending(), "rejected digits leave no state");
  }

  // ---- every word in the table is findable by its own key -------------------------------
  // The property that matters most: if it shipped, you can type it.
  {
    int unreachable = 0, checked = 0;
    char key[64];
    T9Engine e;
    for (int i = 0; i < (int)T9_WORD_COUNT; i += 7) {   // stride: fast but spread across
      keyOf(T9_WORDS[i], key);
      if ((int)std::strlen(key) > T9_MAX_KEY_LEN) continue;
      type(e, key);
      bool found = false;
      for (int c = 0; c < e.candidateCount(); c++) {
        if (std::strcmp(e.candidate(c), T9_WORDS[i]) == 0) {
          found = true;
          break;
        }
      }
      checked++;
      if (!found) {
        if (unreachable < 3) {
          std::printf("  ...unreachable: %s (key %s, %d candidates)\n",
                      T9_WORDS[i], key, e.candidateCount());
        }
        unreachable++;
      }
    }
    std::printf("  reachability: %d/%d sampled words findable\n", checked - unreachable, checked);
    ok(unreachable == 0, "every sampled word is reachable by its own key");
  }

  // ---- candidates are exact-length matches, never prefixes -------------------------------
  {
    T9Engine e;
    char key[64];
    int bad = 0;
    const char* probes[] = { "2", "22", "228", "2273", "43556", "8447", "76737" };
    for (unsigned p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
      type(e, probes[p]);
      for (int c = 0; c < e.candidateCount(); c++) {
        keyOf(e.candidate(c), key);
        if (std::strcmp(key, probes[p]) != 0) {
          bad++;
          std::printf("  ...%s returned %s whose key is %s\n", probes[p], e.candidate(c), key);
        }
      }
    }
    ok(bad == 0, "every candidate's key equals the typed digits exactly");
  }

  // ---- cycling wraps and stays in range --------------------------------------------------
  {
    T9Engine e;
    type(e, "2273");                          // card / case / base / bare in the placeholder
    const int n = e.candidateCount();
    ok(n > 0, "2273 matches something");
    if (n > 0) {
      const char* first = e.current();
      for (int i = 0; i < n; i++) {
        ok(e.current() != NULL, "current() never NULL while candidates exist");
        e.nextCandidate();
      }
      eqs(e.current(), first, "nextCandidate wraps after a full cycle");
      e.prevCandidate();
      eqs(e.current(), e.candidate(n - 1), "prevCandidate from 0 wraps to the end");
      e.nextCandidate();
      eqs(e.current(), first, "and back again");
    }
  }

  // ---- backspace un-types one keypress ---------------------------------------------------
  {
    T9Engine e;
    type(e, "2273");
    ok(e.digitCount() == 4, "four digits pending");
    e.popDigit();
    ok(e.digitCount() == 3, "popDigit removes one digit");
    eqs(e.digits(), "227", "digit buffer shortened correctly");
    e.popDigit();
    e.popDigit();
    e.popDigit();
    ok(!e.pending(), "popping to empty clears pending");
    ok(e.candidateCount() == 0, "and clears candidates");
    ok(!e.popDigit(), "further pops return false");
  }

  // ---- selection resets when the word changes --------------------------------------------
  // If it did not, typing another letter could leave `sel` pointing past the new, shorter run.
  {
    T9Engine e;
    type(e, "2273");
    if (e.candidateCount() > 1) {
      e.nextCandidate();
      ok(e.selected() != 0, "selection moved");
    }
    e.pushDigit('7');
    ok(e.selected() == 0, "pushDigit resets the selection");
    ok(e.selected() < e.candidateCount() || e.candidateCount() == 0, "selection in range");
    type(e, "2273");
    if (e.candidateCount() > 1) {
      e.nextCandidate();
      e.popDigit();
      ok(e.selected() == 0, "popDigit resets the selection");
    }
  }

  // ---- the buffer cannot be overrun -------------------------------------------------------
  {
    T9Engine e;
    e.clear();
    int accepted = 0;
    for (int i = 0; i < 500; i++) {
      if (e.pushDigit('2')) accepted++;
    }
    ok(accepted <= T9_MAX_KEY_LEN, "pushDigit stops at T9_MAX_KEY_LEN");
    ok(e.digitCount() == accepted, "digit count matches what was accepted");
    ok(e.digits()[e.digitCount()] == '\0', "digit buffer stays NUL-terminated");
    while (e.popDigit()) { }
    ok(!e.pending(), "drains cleanly");
  }

  // ---- a key nothing matches is not an error ----------------------------------------------
  {
    T9Engine e;
    type(e, "99999999");                      // zzzzzzzz — nothing spells that
    ok(e.pending(), "digits still pending");
    ok(e.candidateCount() == 0, "no candidates");
    eqs(e.current(), NULL, "current() is NULL — the multi-tap fallback trigger");
    ok(e.popDigit(), "and it can still be backspaced");
  }

  // ---- apostrophes are typed without the symbols row ---------------------------------------
  {
    T9Engine e;
    char key[64];
    int contractions = 0, reachable = 0;
    for (int i = 0; i < (int)T9_WORD_COUNT; i++) {
      if (!std::strchr(T9_WORDS[i], '\'')) continue;
      contractions++;
      keyOf(T9_WORDS[i], key);
      if ((int)std::strlen(key) > T9_MAX_KEY_LEN) continue;
      type(e, key);
      for (int c = 0; c < e.candidateCount(); c++) {
        if (std::strcmp(e.candidate(c), T9_WORDS[i]) == 0) {
          reachable++;
          break;
        }
      }
    }
    std::printf("  contractions: %d in table, %d reachable without the apostrophe key\n",
                contractions, reachable);
    ok(contractions == reachable, "every contraction is reachable from letters alone");
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
