/*
Copyright © 2026 WiPhone Meshtastic firmware contributors.
Licensed under the WiPhone Public License v.1.0 (the "License"); you
may not use this file except in compliance with the License. You may
obtain a copy of the License at
https://wiphone.io/WiPhone_Public_License_v1.0.txt.

Unless required by applicable law or agreed to in writing, software,
hardware or documentation distributed under the License is distributed
on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/

/* T9 predictive text — the engine half. No UI, no widgets, no globals it does not own.
 *
 * 🛑 THIS CLASS ALLOCATES NOTHING, EVER. Every buffer is a fixed member and the word table is
 * memory-mapped flash (src/assets/t9_dict.h), so a lookup performs ZERO heap operations. That
 * is not a nicety: on this phone an internal-heap allocation failure inside `new` throws, and
 * a decoded panic in the field showed the throw itself faulting inside _Unwind_RaiseException
 * before any handler could run. The only safe amount of allocation on the keypress path is
 * none, so `sizeof(T9Engine)` is the entire cost and it is about 40 bytes.
 *
 * HOW IT WORKS
 *   The table is sorted by keypad digit key ("cat" -> "228"), then by descending frequency
 *   within each key. So:
 *     - a binary search finds the first word whose key >= the typed digits;
 *     - the words matching exactly are a CONTIGUOUS RUN from there;
 *     - the run is already in the order the user wants to see it, best first.
 *   Neither the key nor the frequency is stored — the key is recomputed from the word (a
 *   dozen table lookups) and the rank IS the position in the run.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   It does not know about widgets, the keypad, or ControlState. It is given digits and hands
 *   back words. The decision to consume a keypress, and where the resulting text goes, belongs
 *   to the input path; keeping that out of here is what makes this testable off-device.
 */
#ifndef _WIPHONE_T9_H
#define _WIPHONE_T9_H

#include <stdint.h>
#include <stddef.h>

/* The keypad digit for a lowercase letter, or 0 for anything else. Apostrophes deliberately
 * return 0 and are SKIPPED when a key is computed, so "don't" is keyed as "dont" — type
 * d-o-n-t and get the apostrophe for free, exactly as the old phones did. */
char t9DigitForChar(char c);

/* True if `c` is a digit this engine accepts as part of a word ('2'..'9').
 * '0' is space, '1' is punctuation on a real T9 phone, and neither spells anything. */
bool t9IsWordDigit(char c);

class T9Engine {
public:
  T9Engine() {
    clear();
  }

  /* Throw away the pending word. Cheap, and safe to call when nothing is pending. */
  void clear();

  /* Append one keypad digit ('2'..'9') and re-run the lookup.
   * Returns false — and changes nothing — if the digit is not a word digit or the buffer is
   * full. A full buffer is not an error worth reporting to the user: T9_MAX_KEY_LEN is the
   * longest word in the table, so nothing beyond it could match anyway. */
  bool pushDigit(char digit);

  /* Remove the last digit and re-run the lookup. Returns false if nothing was pending.
   *
   * ⚠ BACKSPACE MUST COME HERE, NOT TO THE WIDGET. While a word is pending, one BACK press
   * means "un-type one keypress", which is one DIGIT — not one character of the candidate
   * currently on screen. Deleting a character instead is how the digit buffer and the visible
   * text drift apart, which is the bug this API exists to make hard to write. */
  bool popDigit();

  int digitCount() const {
    return nDigits;
  }
  bool pending() const {
    return nDigits > 0;
  }
  /* The digits typed so far, NUL-terminated. Never NULL. */
  const char* digits() const {
    return digitBuf;
  }

  /* How many words match the typed digits exactly. 0 means the user has typed something this
   * dictionary does not contain — which is not an error, it is the moment to fall back to
   * multi-tap. */
  int candidateCount() const {
    return runLen;
  }

  /* Candidate `i`, best first, or NULL if out of range. Points into flash and stays valid
   * for the life of the firmware, so it is safe to hold without copying. */
  const char* candidate(int i) const;

  /* The candidate currently selected, or NULL when there are none. This is the word that
   * should be showing in the text field. */
  const char* current() const {
    return candidate(sel);
  }

  int selected() const {
    return sel;
  }

  /* Move the selection. Both wrap, so holding DOWN cycles a two-word run forever rather than
   * sticking at the end — measured ambiguity is 1.6 candidates on average, so wrapping is
   * almost always the difference between one press and two. No-ops when nothing matches. */
  void nextCandidate();
  void prevCandidate();

private:
  void lookup();

  /* +1 for the NUL. Sized from the dictionary itself, so it cannot be outgrown by a bigger
   * word list without the header changing too. */
  char     digitBuf[32];
  uint8_t  nDigits;

  uint16_t runStart;      // index of the first matching word in T9_WORDS
  uint16_t runLen;        // how many match; 0 when nothing does
  uint16_t sel;           // which candidate is selected, always < runLen (or 0)
};

#endif // _WIPHONE_T9_H
