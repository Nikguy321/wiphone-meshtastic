#!/usr/bin/env python3
"""check_ai_glue.py - Menu > AI's current facts and its patience keep their rules in the device
files (2026-10-11).

test_gemini and test_wiki prove the pure halves (gemini.cpp: the date sentence, no tool, the
background in the question's turn, the sources line and its strip, rule 0; wiki_lookup.cpp: the
keywords, the URL, the parse, the background). The rules below live in GLUE the host suite cannot
compile - ai_net.cpp, https_worker.cpp/.h - and each is a decision a later edit could undo with
every test still green:

  1. PATIENCE. AI_TIMEOUT_MS is past HTTPClient's uint16 (65,535 ms: free-tier 3.8 Flash took 84 s
     for one sentence) and no more than 5 minutes; postOnce hands it on with waitSliceMs =
     AI_SLICE_MS, and stopFn = tileFetchCallRecent (a call that rings in ends the wait - review
     2026-10-11). NetHttpReq.timeoutMs is a uint32_t (a uint16_t would wrap 150000 to 18,928
     silently), it has a waitSliceMs, a totalMs and a stopFn.
  2. THE CLIENT WAITS, HTTPClient DOES NOT. netHttps makes its client a NetClient, clamps
     http.setTimeout() to NET_HTTP_TIMEOUT_MAX, arms the client (armFirstByte with q->stopFn)
     BEFORE sendRequest under a condition on waitSliceMs, calls the client's own deadline
     GEM_NET_TIMEOUT, checks Cancel / stopFn after DNS, connects within what is left of totalMs,
     and readBody ends on Cancel / stopFn too.
  3. THE WAIT ITSELF (NetClient::available): only once the request is written (m_sent) - before
     that HTTPClient's connect()/sendHeader() call it too, and a wait there holds the request; it
     honours `*m_cancel` AND m_stopFn, sleeps (vTaskDelay - never spins beside the loop task),
     stop()s at the deadline (or HTTPClient starts a 65 s wait of its own) and at a Cancel, and
     never calls the virtual connected() (which reads through available() and would come back
     into the wait). write() arms it only for a WHOLE write.
  4. TODAY'S DATE, FROM A TRUSTED CLOCK, PER REQUEST; THE BACKGROUND IN THE QUESTION'S TURN.
     aiRun builds the system instruction with geminiSystemText(utcNow(), ...) inside its loop,
     before the request is built; utcNow() is ntpClock.getTrustedUtcTime() (0 = no date at all);
     both geminiBuildRequest calls send that text (never the fixed GEM_SYSTEM_INSTRUCTION) and
     s_work->bg as the background. After a request that got no answer, a call ends the question.
  5. THE ANSWER IS SCRUBBED after geminiAnswerText, and its sources line goes only with background.
  6. THE ZONE'S NAME only when it is the offset the clock is on (e.ok, e.minutes * 60 == tzS).
  7. THE WIKIPEDIA LOOKUP (aiWikiLookup): `lookup=off` honoured before anything; the URL built
     from wikiKeywords' output alone; ONE netHttps call and no loop (never retried); the policy
     User-Agent (a contact URL in it); the ISRG X1 + X2 + Root YE pins; the 24 KB cap; a total
     budget of 20 s at most and a connect cap of 10 s; Cancel and a call honoured; NO header, body
     or key - nothing but the keywords leaves. aiRun calls it once, BEFORE the ladder's loop.
  8. NO TOOL. Neither "url_context" nor "tools" nor google_search in any request the code
     builds (gemini.cpp, ai_net.cpp, code only - comments may tell the story).
  9. THE CHAT KEEPS THE PLAIN QUESTION. aiLoopTick folds s_req->question; the background
     (s_work->bg) is read nowhere but in aiWikiLookup and aiRun.

Round 3 (2026-10-11):
 10. "RETRY IF IT TIMES OUT". aiRetryTimeouts reads NVS ("wpmesh") with ON as the absent value;
     aiAsk copies it into the question; aiRun hands it to the ladder right after geminiLadderStart
     and gives geminiLadderNext the attempt's `net` (rule 6 needs it); a request ended by Cancel is
     the user's word, ended BEFORE the ladder can call it a timeout. The background is built once,
     outside the loop (contract 7), so a retry never looks Wikipedia up again.
 11. THE LOOKUP'S ROUND-3 RULES. wikiKeywords is given the topic's earlier questions (all of them -
     it walks back past a follow-up), and the background comes only through wikiCompose with the
     keywords (relevance; all or nothing: no sources line, no background) - never wikiBackground /
     wikiSources directly.
 12. THE ROUND-2 REVIEW. postOnce asks for the heap bar before every Gemini handshake (q.heapCheck),
     and a NOMEM failure says "reboot first" (lowMemText); netHttps clamps HTTPClient's own wait to
     what is left of a budget right before the request (and sends nothing with none left), tells the
     client where the budget ends, and calls a body cut by it TIMEOUT; NetClient keeps a READING
     GUARD after the first byte - a Cancel, a call or the budget's end stop()s it - instead of
     disarming.
 13. THE KEY FOLDER IS WRITE-ONLY ON THE UPLOADER (app_gbc_xfer.cpp, 2026-10-11). The resume
     question (GET /chunk, both transports) goes through xferHeld, which answers 0 for a key file
     (xferKeyPath = files_paths.h's filePathIsKeyFile: an "API Keys" folder, a gemini.txt or a
     chutes.txt) BEFORE any SD.open; chunkOpenFor refuses a resume (off != 0) of a key file with
     409 before it opens one; and nothing in the file opens a file to READ but xferHeld's stat and
     /health.log, nor lists a folder (no openNextFile / SD.exists) - the server may be an open
     hotspot.

D-175 (2026-10-18), Chutes - the second provider, off without the owner's own chutes.txt:
 14. THE BEARER GOES ONLY TO llm.chutes.ai. postOnceChutes builds `Authorization: Bearer <key>` with
     chutesAuthHeader into s_work->auth, sends it as a HEADER to CHUTES_HOST / CHUTES_URL with the
     Chutes root set, and wipes it as soon as netHttps returns; "Authorization", "Bearer" and
     chutesAuthHeader appear NOWHERE else in ai_net.cpp's code (not Gemini's postOnce, not the
     lookup); Gemini's postOnce sends no Chutes key and postOnceChutes no Gemini one; CHUTES_HOST is
     "llm.chutes.ai" and the URL is https to it; netHttps never follows a redirect (setFollowRedirects
     appears nowhere); the Chutes request keeps Gemini's patience rules (the heap bar, the slices,
     Cancel, a call, the shared sink) with the PRESET's wait.
 15. THE CHUTES ROOTS PINNED. AI_CHUTES_ROOTS_PEM is exactly GoDaddy G2 + ISRG X1 + X2 + Root YR +
     Root YE + GTS R1 + R4 (test_chutes checks each SHA-256; YR / YE since the review: a Let's
     Encrypt renewal served with the short Gen-Y chain anchors there) and Gemini's AI_ROOTS_PEM is
     still GTS R1 + R4.
 16. CHUTES' LOOP KEEPS THE RULES (aiRunChutes): the system text per request from utcNow(); two
     chutesBuildRequest calls with the plain question, the lookup's background and the built text;
     a fresh seed per attempt from esp_random(); Cancel and a call told apart after the request,
     before the ladder; the reply parsed as the attempt's preset's (l.cur: its thinking switch);
     the ladder given the attempt's code and net; the answer scrubbed with the CHUTES key after
     chutesAnswerText, its sources line only with background; no lookup of its own - aiRun calls
     it once, AFTER its one aiWikiLookup, and hands the ladder the retry setting. Review
     2026-10-18: aiRun's Chutes branch RETURNS, unconditionally, before Gemini's loop (never Gemini
     in place of Chutes, nor a key-less POST to Google after one); "Chutes has answered" is set on
     DONE and handed to the ladder as !firstOfBoot; the gone presets are saved after the loop and
     handed to the next question's chutesLadderStart. The X4's review (2026-10-19): chutesAnswerText
     is given l.cur, the preset that answered (its declined note names it). The WiPhone's review
     (2026-10-19, COVEY's rule): the answer is built ONCE, by chutesAnswerShown, after the parse and
     BEFORE the ladder - "any text" is what the screen shows, so an emoji alone with an "abort" is
     busy, never "sent an empty answer".
 17. NO KEY IN ANY LOG, SCREEN OR CONSOLE LINE, NO CHUTES TEXT EITHER, AND THE CHOICE COPIED AT
     ASK TIME. No log_*, snprintf, emit or progSet in ai_net.cpp is given a key field or the header;
     and (review 2026-10-18: that list matched names, and four leaks passed) EVERY read of a key
     field (`.key` / `->key`, any struct), of the header (s_work->auth), and - in the two reloads -
     of the file's buffer and the parse's local is one of an allowlist: the wipes, sizeof, the
     header's build and its one assignment, the scrub, the failure words, the parse calls. Key
     info's lines are written only by the *ConfigLine calls or a literal. The raw body
     (netWorkerSink(): a 402 names a wallet) and the answer's raw text (s_work->text) go only into
     the parsers and the answer builders - never a failure text, a label, a log. AiKeyInfo and
     AiChutesKey carry no key; app_ai.cpp and serial_cmd.cpp never parse a key file; `ai model`
     never echoes what was typed; gemini.txt is parsed by geminiParseConfig (which refuses a cpk_
     key), never by the provider-blind reader. aiAsk copies aiModelChoice() into s_req->choice and
     only that provider's key; aiJob and aiLoopTick wipe the Chutes copy (aiJob the header too);
     the choice is chutesChoiceResolve over NVS "ai_model" (isKey first: an absent key logs
     nothing), and aiSetModelChoice stores only an allowlisted id; chutes.txt is read from
     AI_CHUTES_FILE only. The default rule's DEVICE inputs (review 2026-10-18): both reloads' latch
     (a card hiccup keeps a key only after a good read; an unreadable file is usable only after
     one; a refused file drops the kept key), aiChoiceUsable's provider split, and aiAsk's gate.

Review 2026-10-18 (D-175's fixes):
 18. THE WIPES THE COMPILER KEEPS. No memset() right before free() in ai_net.cpp (gcc drops it as a
     dead store - measured in ai_net.cpp.o); the reloads' parse and buffer, and both loops' reply,
     are wiped with geminiWipe (volatile stores, gemini.cpp) right before their free().
 19. THE SCREENS NEVER TRAP (app_ai.cpp). Key info's Back leaves the app only with NO key at all and
     no chat (not "the model in use has none"); Key info offers a "Model:" row (ROW_MODEL -> MODELS)
     when the model in use has no key and the other provider's has; the uploader's green line asks
     the model in use (choiceUsable); OK on the picker's row in use stores nothing.
 20. KEY FILES ARE NEVER CLONED (tools/card_clone.sh). Every key-file name files_paths.h's
     filePathIsKeyFile knows - the "API Keys" folder, gemini.txt, chutes.txt - is excluded from the
     rsync (any case) AND from the completeness count, and named in the usage line.

The X4's review (2026-10-19), brought back to the phone:
 21. A TOPIC NEVER SPANS GEMINI AND CHUTES (Key info: "never to Google"). aiAsk's s_req->topic is
     chutesTopicDue(chat, s_req->askedUtc, choice, s_m->topicLast) - the provider of this boot's last
     question and the labels of what would be sent, host-tested in test_chutes - set once; the
     context is copied only when it is not a topic; s_m->topicLast takes the question's provider
     after that and after the last refusal; it starts UNKNOWN (calloc's 0 would be Gemini). aiTopicDue
     asks the same rule with the same inputs (aiModelChoice(), s_m->topicLast), and `ai` and Menu > AI
     ask it: the menu's CHUTES_TOPIC_SWITCH note (and `ai model`'s line) shows when only the
     provider's rule says a topic starts. geminiChatTopicDue (provider-blind) decides nothing in ai_net.cpp, app_ai.cpp or
     serial_cmd.cpp - aiTopicDue's "only that reason" line alone asks it. And (contract 16) the answer's
     words are given the preset that ANSWERED (l.cur): a content_filter after text names it.

D-176 (2026-10-19), the web search - off without the owner's own tavily.txt:
 22. THE BEARER GOES ONLY TO api.tavily.com. aiWebSearch builds `Authorization: Bearer <key>` with
     webAuthHeader(q->web.key, s_work->webAuth, ...) before its ONE netHttps (no loop: never retried)
     and wipes it right after; sends it as a HEADER to WEB_HOST / WEB_URL ("api.tavily.com",
     https://WEB_HOST/search) with the Tavily root set, POST, the body s_work->webBody (built from the
     plain question), the handshake timed alone under AI_WEB_CONNECT_MS (<= 10 s), Tavily's answer
     under WEB_ANSWER_MS (6000: 6 s since 2026-10-19; the spec said 3 s), the whole under AI_WEB_TOTAL_MS = the two, the slices,
     Cancel, a call, the heap bar, and the reply parsed by the tap as it arrives (cap 0: nothing raw
     kept; webTap only feeds webParseFeed). "Authorization" appears only in postOnceChutes and
     aiWebSearch, webAuthHeader and s_work->webAuth only there (and the wipes); no other request
     (Gemini's, Chutes', the lookup) carries the Tavily key, and the search carries no other key.
 23. THE TAVILY ROOTS PINNED. AI_TAVILY_ROOTS_PEM is exactly Amazon Root CA 1 + Starfield Services
     Root G2 (test_websearch checks each SHA-256), and amazon_roots.h records both fingerprints.
 24. THE DECISION, THE KEY AND THE COUNT. aiAsk asks webDecide with the setting, a GOOD tavily.txt
     read (aiWebUsable: wkey.usable && s_wcfg && s_wHaveGood), the per-question force, the plain
     question and today's automatic count from NVS (dayLoad first), and copies the Tavily key ONLY when
     it searches; aiWebTest refuses under Off and without a key and carries no other key; aiRun makes
     the search ONCE after the one lookup and before either provider (the bench-only branch returns
     before any ladder); aiLoopTick counts a SENT search only (webDayCount + daySave under
     s_res->webSent, Auto's apart) before anything else, and aiWebSearch marks it sent only once the
     request was written (res.largestAfterSend != 0 || code > 0: DNS, a refused handshake or the heap
     bar spend no credit - review 2026-10-19); the day is a trusted clock's (utcNow()); the NVS names are ai_web /
     ai_webd / ai_webn / ai_webo; webModeLoad asks isKey first; aiSetWebMode stores an allowlisted id;
     aiJob and aiLoopTick wipe the key copy, aiJob the header; tavily.txt is read from AI_TAVILY_FILE
     only, through webParseConfig (geminiReadKeyFile's rules, tvly- only); gemini.txt refuses a tvly-
     key; `ai web` echoes nothing typed; Menu > AI offers "Ask with web search" only with a key and
     not Off, its row cycles only with a key, OK on it opens the compose with the flag set (handled with
     ROW_ASK: askWeb = k == ROW_ASKWEB && next == AI_COMPOSE, before enterState(next) - review
     2026-10-19: either half broken, the row did nothing or asked without a search), and the compose's
     flag dies with its compose and reaches aiAsk.
 25. THE BLOCK IS NEVER STORED. s_work->web / webSrc / webBody are read only in aiWebSearch - and the
     block by aiWebReport for `ai web test` alone (after its webOnly guard); the background, the block
     and the body are in no log, no printed or saved text and no chat fold; s_work->src is written
     only by wikiCompose, webAppendLine and the clears (so "" is no line, and the answer's sources are
     `s_work->src[0] ? s_work->src : NULL`); aiWikiLookup clears bg and src before anything. (aiWebTest
     may only WIPE web and webSrc: contract 27.)
 26. THE SEARCH IS SEEN (review 2026-10-19). aiWebSearch sets the stage to WEB_STAGE ("Searching the
     web...") before its netHttps; every way out says why under the answer: the no-search path appends
     the cap's note (r->webNote = q->webNote, appended when not NONE), the no-body/no-key path sets
     UNAVAILABLE and appends its note, and the last line appends `note == WEB_NOTE_NONE ? s_work->webSrc
     : webNoteLine(note)` after r->webNote = note; aiAsk turns WEB_WHY_CAP into WEB_NOTE_CAP.
 27. A BENCH SEARCH LEAVES NOTHING BEHIND (review 2026-10-19). aiLoopTick's webOnly branch resets the
     Clear chat / New topic latches (s_breakAfter, s_clearAfter) - left set, the NEXT question's answer
     was dropped or its topic broken again; aiWebTest wipes s_work->web and s_work->webSrc before it
     submits (a test that ends at the worker's gate never searched); aiWebReport prints a block only for
     a search that ran and put one in (s_res->webAsked && webNote == WEB_NOTE_NONE); aiWebSearch wipes a
     QUESTION's block copy once it is in bg (no question's block outlives it where `ai web` could print).

A contract that cannot find its file or function fails too. The self-test first re-applies each
mistake to an in-memory copy of the real sources and requires the check to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("ai_net.cpp", "https_worker.cpp", "https_worker.h", "wiki_lookup.h", "gemini.cpp", "app_gbc_xfer.cpp",
         "chutes.h", "chutes.cpp", "ai_net.h", "app_ai.cpp", "serial_cmd.cpp", "files_paths.h",
         "websearch.h", "websearch.cpp", "amazon_roots.h", "gemini.h")
CLONE = "tools/card_clone.sh"                          # contract 20 (keyed by its path from the repo)

_STRIPPED = {}


def stripped(text):
    key = hash(text)
    if key not in _STRIPPED:
        _STRIPPED[key] = strip_code(text)
    return _STRIPPED[key]


def load():
    texts = {name: (ROOT / name).read_text(encoding="utf-8", errors="replace") for name in FILES}
    texts[CLONE] = (REPO / CLONE).read_text(encoding="utf-8", errors="replace")
    return texts


def body(code, fn, out, where):
    span = function_body(code, fn)
    if not span:
        out.append("%s: %s not found" % (where, fn))
    return span


def args_of(code, start, end, name):
    """The argument text of every call `name(...)` in code[start:end], whitespace squeezed."""
    out = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), code[start:end]):
        po = start + m.end() - 1
        pc = match_close(code, po)
        if pc > 0:
            out.append((start + m.start(), re.sub(r"\s+", " ", code[po + 1:pc]).strip()))
    return out


def define(raw, name):
    """The value text of `#define name ...` in the RAW source (string literals intact; backslash
    continuation lines joined), or None."""
    m = re.search(r"^#define\s+%s\s+((?:.*\\\n)*.*)$" % re.escape(name), raw, re.M)
    return re.sub(r"\\\n", " ", m.group(1)).strip() if m else None


def call_at(code, pos):
    """The innermost call (or parenthesised group) around code[pos], as (name, [(start, end) of each
    top-level argument], the index of the one holding pos) - `name` is the word before its '('
    ("if" for a condition, "" for a bare group) - or None when pos stands at statement level."""
    depth, i = 0, pos - 1
    while i >= 0:
        ch = code[i]
        if ch == ")":
            depth += 1
        elif ch == "(":
            if depth == 0:
                break
            depth -= 1
        elif ch in ";{}" and depth == 0:
            return None
        i -= 1
    if i < 0:
        return None
    po, pc = i, match_close(code, i)
    if pc < 0:
        return None
    m = re.search(r"([A-Za-z_]\w*(?:\s*(?:\.|->)\s*\w+)*)\s*$", code[max(0, po - 96):po])   # a window: speed
    name = re.sub(r"\s+", "", m.group(1)) if m else ""
    spans, start, d = [], po + 1, 0
    for j in range(po + 1, pc):
        c = code[j]
        if c in "([{":
            d += 1
        elif c in ")]}":
            d -= 1
        elif c == "," and d == 0:
            spans.append((start, j))
            start = j + 1
    spans.append((start, pc))
    idx = next((k for k, (a, b) in enumerate(spans) if a <= pos < b), None)
    return name, spans, idx


def statement_at(code, pos):
    """The statement around code[pos] (from the last ; { or } before it to the next ;), squeezed."""
    a = max(code.rfind(ch, 0, pos) for ch in ";{}") + 1
    b = code.find(";", pos)
    return re.sub(r"\s+", " ", code[a:b if b >= 0 else len(code)]).strip()


def line_of(code, pos):
    return code.count("\n", 0, pos) + 1


def uses_allowed(code, rx, allowed, where, what, out, start=0, end=None, statements=(), skip=None):
    """Every match of `rx` in code[start:end] must sit in an allowed call - (name, argument index or
    None for any) - or, at statement level, in one of `statements` (regexes over the squeezed
    statement). `skip(m)` True: not a use (another thing of the same name)."""
    end = len(code) if end is None else end
    for m in rx.finditer(code, start, end):
        if skip and skip(m):
            continue
        c = call_at(code, m.start())
        if c is not None:
            name, spans, idx = c
            if any(name == n and (k is None or k == idx) for n, k in allowed):
                continue
            if name == "if" and any(re.fullmatch(s, re.sub(r"\s+", " ", code[spans[0][0]:spans[0][1]]).strip())
                                    for s in statements):
                continue
            out.append("%s:%d: %s in %s(...) - %s" % (where, line_of(code, m.start()), m.group(0).strip(),
                                                      name or "(...)", what))
        elif not any(re.fullmatch(s, statement_at(code, m.start())) for s in statements):
            out.append("%s:%d: %s in `%s` - %s" % (where, line_of(code, m.start()), m.group(0).strip(),
                                                   statement_at(code, m.start())[:70], what))


def check(texts):
    out = []
    code = {k: stripped(v) for k, v in texts.items() if k != CLONE}
    ai, hw, hh = code["ai_net.cpp"], code["https_worker.cpp"], code["https_worker.h"]

    # 1. the timeout and the request fields
    m = re.search(r"#define\s+AI_TIMEOUT_MS\s+(\d+)u?\b", ai)
    if not m:
        out.append("ai_net.cpp: AI_TIMEOUT_MS not found")
    else:
        v = int(m.group(1))
        if v <= 65535 or v > 300000:
            out.append("ai_net.cpp: AI_TIMEOUT_MS %d - must be past HTTPClient's 65535 and at most 5 min" % v)
    span = body(ai, "postOnce", out, "ai_net.cpp")
    if span:
        s, e = span
        if not re.search(r"\bq\s*\.\s*timeoutMs\s*=\s*AI_TIMEOUT_MS\s*;", ai[s:e]):
            out.append("postOnce: q.timeoutMs is not AI_TIMEOUT_MS")
        if not re.search(r"\bq\s*\.\s*waitSliceMs\s*=\s*AI_SLICE_MS\s*;", ai[s:e]):
            out.append("postOnce: q.waitSliceMs = AI_SLICE_MS missing - the wait would not honour Cancel in slices")
        if not re.search(r"\bq\s*\.\s*stopFn\s*=\s*tileFetchCallRecent\s*;", ai[s:e]):
            out.append("postOnce: q.stopFn = tileFetchCallRecent missing - a ringing call would wait behind the answer")
        if not re.search(r"\bq\s*\.\s*heapCheck\s*=\s*true\s*;", ai[s:e]):
            out.append("postOnce: q.heapCheck = true missing - a heap fragmented by the lookup's handshake would fail generically")
    if not re.search(r"\buint32_t\s+timeoutMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq.timeoutMs is not a uint32_t (150000 would wrap)")
    if not re.search(r"\buint32_t\s+waitSliceMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no uint32_t waitSliceMs")
    if not re.search(r"\buint32_t\s+totalMs\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no uint32_t totalMs")
    if not re.search(r"\bNetStopFn\s+stopFn\s*;", hh):
        out.append("https_worker.h: NetHttpReq has no NetStopFn stopFn")
    if not re.search(r"#define\s+NET_HTTP_TIMEOUT_MAX\s+65535u?\b", hh):
        out.append("https_worker.h: NET_HTTP_TIMEOUT_MAX is not 65535 (HTTPClient's uint16)")

    # 2. netHttps
    span = body(hw, "netHttps", out, "https_worker.cpp")
    if span:
        s, e = span
        h = hw[s:e]
        if not re.search(r"\bNetClient\s*\*\s*c\s*=\s*new\s*\(\s*std\s*::\s*nothrow\s*\)\s*NetClient\s*\(", h):
            out.append("netHttps: the client is not a NetClient - nothing can wait past 65 s")
        send = re.search(r"\bhttp\s*\.\s*sendRequest\s*\(", h)
        st = args_of(hw, s, e, "setTimeout")
        if not st or "NET_HTTP_TIMEOUT_MAX" not in st[0][1]:
            out.append("netHttps: http.setTimeout() is not clamped to NET_HTTP_TIMEOUT_MAX (a uint16 would wrap)")
        # 12. the budget's clamp, right before the request
        restm = re.search(r"\bconst\s+uint32_t\s+rest\s*=\s*netLeft\s*\(\s*q\s*,\s*t0\s*\)\s*;", h)
        clamp = re.search(r"\bif\s*\(\s*q\s*->\s*totalMs\s*\)\s*\{\s*uint32_t\s+hw\s*=\s*q\s*->\s*timeoutMs\s*<\s*"
                          r"NET_HTTP_TIMEOUT_MAX\s*\?[^;]*;\s*if\s*\(\s*rest\s*<\s*hw\s*\)\s*\{\s*hw\s*=\s*rest\s*;\s*\}\s*"
                          r"http\s*\.\s*setTimeout\s*\(\s*\(\s*uint16_t\s*\)\s*hw\s*\)\s*;", h)
        if not restm or not clamp or not send or not (restm.start() < clamp.start() < send.start()):
            out.append("netHttps: HTTPClient's wait is not clamped to what is left of a budget right before the request")
        if len(st) != 2:
            out.append("netHttps: %d setTimeout calls - the default at the start and the budget's clamp" % len(st))
        if not re.search(r"\bif\s*\(\s*connected\s*&&\s*rest\s*==\s*0\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;", h):
            out.append("netHttps: a budget spent before the request still sends it")
        if not re.search(r"\bif\s*\(\s*q\s*->\s*totalMs\s*\)\s*\{\s*c\s*->\s*budgetEnds\s*\(\s*t0\s*\+\s*q\s*->\s*totalMs\s*\)\s*;", h):
            out.append("netHttps: the client is not told where the budget ends (the reading guard would not end it)")
        if not re.search(r"\bif\s*\(\s*c\s*->\s*timedOut\s*\(\s*\)\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;[^}]*\}\s*code\s*=\s*0\s*;", h):
            out.append("netHttps: a body cut at the budget's end is not called TIMEOUT")
        arm = re.search(r"\bc\s*->\s*armFirstByte\s*\(\s*\w+\s*,\s*q\s*->\s*waitSliceMs\s*,\s*q\s*->\s*cancel\s*,"
                        r"\s*q\s*->\s*stopFn\s*\)", h)
        send = re.search(r"\bhttp\s*\.\s*sendRequest\s*\(", h)
        if not arm:
            out.append("netHttps: the client is never armed (armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn))")
        elif not send or arm.start() > send.start():
            out.append("netHttps: armFirstByte is not before sendRequest - the wait would never run")
        else:
            # ...under an `if (` whose condition names waitSliceMs
            before = hw[s:s + arm.start()]
            ifm = list(re.finditer(r"\bif\s*\(", before))
            okc = False
            if ifm:
                po = s + ifm[-1].end() - 1
                pc = match_close(hw, po)
                okc = pc > 0 and "waitSliceMs" in hw[po:pc]
            if not okc:
                out.append("netHttps: armFirstByte is not under a condition on q->waitSliceMs (the weather would wait too)")
        if not re.search(r"\belse\s+if\s*\(\s*c\s*->\s*timedOut\s*\(\s*\)\s*\)\s*\{\s*r\s*->\s*net\s*=\s*GEM_NET_TIMEOUT\s*;", h):
            out.append("netHttps: the client's own deadline is not GEM_NET_TIMEOUT ('did not answer in time')")
        dns = re.search(r"\bWiFi\s*\.\s*hostByName\s*\(", h)
        stp = re.search(r"\bif\s*\(\s*netStopped\s*\(\s*q\s*\)\s*\)", h)
        if not dns or not stp or stp.start() < dns.start() or (send and stp.start() > send.start()):
            out.append("netHttps: Cancel / stopFn is not checked after DNS, before the handshake")
        if re.search(r"\bsetConnectTimeout\s*\([^;]*\bq\s*->\s*connectMs|\bc\s*->\s*connect\s*\([^;]*\bq\s*->\s*connectMs", h):
            out.append("netHttps: a connect uses q->connectMs, not the budget-clamped connectMs")
        if not re.search(r"\bconnectMs\s*=\s*q\s*->\s*connectMs\s*<\s*left\s*\?", h):
            out.append("netHttps: the connect is not shortened to what is left of totalMs")
        rb = args_of(hw, s, e, "readBody")
        if not rb or not all(a.endswith("t0") for _, a in rb):
            out.append("netHttps: readBody is not given t0 (the body would not end at totalMs)")
    span = body(hw, "readBody", out, "https_worker.cpp")
    if span:
        s, e = span
        if not re.search(r"\bif\s*\(\s*netStopped\s*\(\s*q\s*\)\s*\)", hw[s:e]):
            out.append("readBody: Cancel / stopFn not honoured between reads")
        if not re.search(r"\bnetLeft\s*\(\s*q\s*,\s*t0\s*\)\s*==\s*0", hw[s:e]):
            out.append("readBody: the totalMs budget does not end the body")
    span = body(hw, "netStopped", out, "https_worker.cpp")
    if span and not re.search(r"q\s*->\s*stopFn\s*&&\s*q\s*->\s*stopFn\s*\(\s*\)", hw[span[0]:span[1]]):
        out.append("netStopped: does not ask q->stopFn")

    # 3. NetClient
    cls = re.search(r"\bclass\s+NetClient\s*:\s*public\s+WiFiClientSecure\s*\{", hw)
    if not cls:
        out.append("https_worker.cpp: class NetClient : public WiFiClientSecure not found")
    else:
        cs, ce = cls.end() - 1, match_close(hw, cls.end() - 1)
        cl = hw[cs:ce]
        av = function_body(cl, "available")
        if not av:
            out.append("NetClient: available() not found")
        else:
            a = cl[av[0]:av[1]]
            gate = re.search(r"\bif\s*\(\s*!\s*m_armed\s*\|\|\s*!\s*m_sent\s*\)\s*\{\s*return\s+n\s*;", a)
            loop = re.search(r"\bfor\s*\(\s*;\s*;\s*\)|\bwhile\s*\(", a)
            if not gate or (loop and gate.start() > loop.start()):
                out.append("NetClient::available: the wait is not gated on m_sent (it would hold the request itself)")
            if not re.search(r"\bif\s*\(\s*\(\s*m_cancel\s*&&\s*\*\s*m_cancel\s*\)\s*\|\|\s*\(\s*m_stopFn\s*&&\s*m_stopFn\s*\(\s*\)\s*\)\s*\)"
                             r"\s*\{[^}]*\bstop\s*\(\s*\)\s*;", a):
                out.append("NetClient::available: a Cancel or a call (m_stopFn) does not end the wait (stop())")
            if not re.search(r"\bm_timedOut\s*=\s*true\s*;\s*stop\s*\(\s*\)\s*;", a):
                out.append("NetClient::available: the deadline does not stop() the client (HTTPClient would wait 65 s more)")
            if not re.search(r"\bvTaskDelay\s*\(", a):
                out.append("NetClient::available: the wait does not sleep - it would spin beside the loop task")
            # 12. the reading guard (after the first byte), before the wait's loop
            gd = re.search(r"\bif\s*\(\s*m_answered\s*\)\s*\{", a)
            if not gd or (loop and gd.start() > loop.start()):
                out.append("NetClient::available: no reading guard after the first byte (if (m_answered) {...})")
            else:
                g = a[gd.start():match_close(a, gd.end() - 1)]
                if not re.search(r"\btold\s*=\s*\(\s*m_cancel\s*&&\s*\*\s*m_cancel\s*\)\s*\|\|\s*\(\s*m_stopFn\s*&&\s*m_stopFn\s*\(\s*\)\s*\)", g):
                    out.append("NetClient: the reading guard does not honour a Cancel AND a call")
                if not re.search(r"\bspent\s*=\s*m_hasEnd\s*&&", g):
                    out.append("NetClient: the reading guard does not end at the budget")
                if not re.search(r"\bif\s*\(\s*told\s*\|\|\s*spent\s*\)\s*\{[^}]*\bstop\s*\(\s*\)\s*;\s*return\s+0\s*;", g):
                    out.append("NetClient: the reading guard does not stop() the client")
            if re.search(r"\bm_armed\s*=\s*false\s*;\s*//\s*the answer began", a) or \
                    len(re.findall(r"\bm_answered\s*=\s*true\s*;", a)) < 2:
                out.append("NetClient::available: the first byte disarms the client - no guard while the answer is read")
            if re.search(r"(?<![\w:])connected\s*\(", a) or re.search(r"\bWiFiClientSecure\s*::\s*connected\s*\(", a):
                out.append("NetClient::available: calls connected(), which reads through available() - back into the wait")
        wr = function_body(cl, "write")
        if not wr:
            out.append("NetClient: write() not found")
        elif not re.search(r"\bif\s*\(\s*size\s*>\s*0\s*&&\s*n\s*==\s*size\s*\)\s*\{[^}]*\bm_sent\s*=\s*true",
                           cl[wr[0]:wr[1]]):
            out.append("NetClient::write: the wait is armed by a write that was not whole")

    # 4 + 5. aiRun
    span = body(ai, "aiRun", out, "ai_net.cpp")
    if span:
        s, e = span
        lp = re.search(r"\bfor\s*\(\s*;\s*;\s*\)\s*\{", ai[s:e])
        sysc = args_of(ai, s, e, "geminiSystemText")
        builds = args_of(ai, s, e, "geminiBuildRequest")
        if not sysc:
            out.append("aiRun: geminiSystemText is not called - no date")
        else:
            pos, a = sysc[0]
            if not a.startswith("utcNow()"):
                out.append("aiRun: geminiSystemText is not given utcNow() (the TRUSTED clock): %s" % a)
            if not lp or pos < s + lp.start():
                out.append("aiRun: geminiSystemText is outside the request loop - the date would not be per request")
            if builds and pos > builds[0][0]:
                out.append("aiRun: geminiSystemText comes after the request is built")
        if len(builds) != 2:
            out.append("aiRun: geminiBuildRequest called %d times (the length, then the body)" % len(builds))
        for _, a in builds:
            parts = [p.strip() for p in a.split(",")]
            if len(parts) < 6 or parts[2] != "q->question" or parts[3] != "s_work->bg" or parts[4] != "s_work->sys":
                out.append("aiRun: geminiBuildRequest(%s) - not the plain question, the lookup's background and "
                           "the built instruction" % a)
        if re.search(r"\bGEM_SYSTEM_INSTRUCTION\b", ai[s:e]):
            out.append("aiRun: sends the fixed GEM_SYSTEM_INSTRUCTION (no date)")
        at = args_of(ai, s, e, "geminiAnswerText")
        sc = re.search(r"\bgeminiScrub\s*\(\s*r\s*->\s*answer\s*,", ai[s:e])
        if not sc or not at or sc.start() + s < at[0][0]:
            out.append("aiRun: the answer is not scrubbed after geminiAnswerText")
        if at and "s_work->src[0] ? s_work->src : NULL" not in at[0][1]:
            out.append("aiRun: the sources line is not s_work->src's own (Wikipedia's with its background, then the "
                       "web's line or note - D-176): %s" % at[0][1])
        wl = args_of(ai, s, e, "aiWikiLookup")
        if len(wl) != 1:
            out.append("aiRun: aiWikiLookup called %d times - once a question, never per retry" % len(wl))
        elif not lp or wl[0][0] > s + lp.start():
            out.append("aiRun: aiWikiLookup is inside the request loop - it would be retried")
        post = re.search(r"\bpostOnce\s*\(", ai[s:e])
        call = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*!\s*s_cancel\s*&&\s*tileFetchCallRecent\s*\(\s*\)\s*\)", ai[s:e])
        if not call or not post or call.start() < post.start():
            out.append("aiRun: a request ended by a call is not told apart after postOnce (it would read 'connection dropped')")
        # 10. rule 6's inputs
        start = re.search(r"\bgeminiLadderStart\s*\(", ai[s:e])
        rt = re.search(r"\bl\s*\.\s*retryTimeouts\s*=\s*q\s*->\s*retryTimeouts\s*;", ai[s:e])
        if not rt or not start or rt.start() < start.start() or (lp and rt.start() > lp.start()):
            out.append("aiRun: the ladder is not given the setting right after geminiLadderStart (l.retryTimeouts = q->retryTimeouts)")
        nx = args_of(ai, s, e, "geminiLadderNext")
        if len(nx) != 1 or [x.strip() for x in nx[0][1].split(",")][3:5] != ["code", "net"]:
            out.append("aiRun: geminiLadderNext is not given the attempt's code and net (rule 6 needs why)")
        cn = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*s_cancel\s*\)\s*\{\s*r\s*->\s*cancelled\s*=\s*true\s*;\s*break\s*;", ai[s:e])
        if not cn or not post or cn.start() < post.start() or (nx and s + cn.start() > nx[0][0]):
            out.append("aiRun: a request ended by Cancel is not ended before the ladder (rule 6 would retry it)")
        nm = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*net\s*==\s*GEM_NET_NOMEM\s*\)\s*\{\s*lowMemText\s*\(\s*r\s*->\s*fail\s*,", ai[s:e])
        if not nm:
            out.append("aiRun: a NOMEM failure does not say 'reboot first' (lowMemText)")
    span = body(ai, "utcNow", out, "ai_net.cpp")
    if span and not re.search(r"\bntpClock\s*\.\s*getTrustedUtcTime\s*\(\s*\)", ai[span[0]:span[1]]):
        out.append("utcNow: not ntpClock.getTrustedUtcTime() - a mesh-set clock would date the question")

    # 6. aiAsk's zone name
    span = body(ai, "aiAsk", out, "ai_net.cpp")
    if span:
        s, e = span
        if not re.search(r"\bif\s*\(\s*e\s*\.\s*ok\s*&&\s*e\s*\.\s*name\s*&&\s*e\s*\.\s*minutes\s*\*\s*60\s*==\s*s_req\s*->\s*tzS\s*\)",
                         ai[s:e]):
            out.append("aiAsk: the zone's name is not tied to the clock's offset (e.ok, e.minutes * 60 == tzS)")
        if not re.search(r"\bs_req\s*->\s*retryTimeouts\s*=\s*aiRetryTimeouts\s*\(\s*\)\s*;", ai[s:e]):
            out.append("aiAsk: the question does not carry \"Retry if it times out\" (s_req->retryTimeouts)")
    span = body(ai, "aiRetryTimeouts", out, "ai_net.cpp")
    if span:
        rw = strip_comments(texts["ai_net.cpp"][span[0]:span[1]])
        if not re.search(r"\bgetUChar\s*\(\s*AI_PREFS_RETRY\s*,\s*1\s*\)", rw):
            out.append("aiRetryTimeouts: absent is not ON (getUChar(AI_PREFS_RETRY, 1))")
    if ((define(texts["ai_net.cpp"], "AI_PREFS_NS") or "").split() or [""])[0] != '"wpmesh"':
        out.append("ai_net.cpp: AI_PREFS_NS is not the firmware's \"wpmesh\" namespace")

    # 7. the lookup
    raw_ai, raw_wh = texts["ai_net.cpp"], texts["wiki_lookup.h"]
    span = body(ai, "aiWikiLookup", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        off = re.search(r"\bif\s*\(\s*!\s*q\s*->\s*cfg\s*\.\s*lookup\s*\)\s*\{[^}]*\breturn\s*;", w)
        kwc = re.search(r"\bwikiKeywords\s*\(", w)
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        if not off or (kwc and off.start() > kwc.start()) or (net and off.start() > net[0].start()):
            out.append("aiWikiLookup: lookup=off is not honoured before anything else")
        if not re.search(r"\bwikiKeywords\s*\(\s*q\s*->\s*question\s*,", w):
            out.append("aiWikiLookup: the keywords are not the question's (wikiKeywords(q->question, ...))")
        # 11. the walk back and the all-or-nothing composer
        kwa = args_of(ai, s, e, "wikiKeywords")
        if not kwa or [x.strip() for x in kwa[0][1].split(",")][1:3] != ["earlier", "nEarlier"] or \
                not re.search(r"\bconst\s+int\s+nEarlier\s*=\s*topicQuestions\s*\(\s*q\s*,\s*earlier\s*\)\s*;", w):
            out.append("aiWikiLookup: wikiKeywords is not given the topic's earlier questions (the walk back)")
        cp = args_of(ai, s, e, "wikiCompose")
        if len(cp) != 1 or [x.strip() for x in cp[0][1].split(",")][:2] != ["&s_work->wiki", "s_work->kw"]:
            out.append("aiWikiLookup: the background does not come through wikiCompose(&s_work->wiki, s_work->kw, ...) - relevance")
        if re.search(r"\bwikiBackground\s*\(|\bwikiSources\s*\(", w):
            out.append("aiWikiLookup: wikiBackground/wikiSources called directly - past relevance and all-or-nothing")
        if not re.search(r"\bwikiUrl\s*\(\s*s_work\s*->\s*kw\s*,\s*s_work\s*->\s*wikiUrl\s*,", w):
            out.append("aiWikiLookup: the URL is not built from the keywords alone (wikiUrl(s_work->kw, ...))")
        if not re.search(r"\bw\s*\.\s*url\s*=\s*s_work\s*->\s*wikiUrl\s*;", w):
            out.append("aiWikiLookup: the request's URL is not the one built from the keywords")
        if len(net) != 1:
            out.append("aiWikiLookup: %d netHttps calls - exactly ONE GET a question" % len(net))
        if re.search(r"\bfor\s*\(|\bwhile\s*\(|\bgoto\b", w):
            out.append("aiWikiLookup: a loop - the lookup is never retried")
        rw = strip_comments(raw_ai[s:e])            # the same function, string literals intact
        if not re.search(r'\bw\s*\.\s*method\s*=\s*"GET"\s*;', rw):
            out.append("aiWikiLookup: not a GET")
        if re.search(r"\bhdrName\b|\bhdrValue\b|\bcfg\s*\.\s*key\b|\bchutes\s*\.\s*key\b|\bauth\b|\bw\s*\.\s*body\b|\bctxA\b", w) or \
                "x-goog-api-key" in rw or "Authorization" in rw or "Bearer" in rw:
            out.append("aiWikiLookup: a header, a body, the key or the chat in the request - only the keywords may go")
        for field, want in (("userAgent", "WIKI_USER_AGENT"), ("rootsPem", "AI_WIKI_ROOTS_PEM"),
                            ("cap", "WIKI_RESPONSE_CAP"), ("totalMs", "AI_WIKI_TOTAL_MS"),
                            ("connectMs", "AI_WIKI_CONNECT_MS"), ("stopFn", "tileFetchCallRecent"),
                            ("waitSliceMs", "AI_SLICE_MS"), ("sink", "netWorkerSink ( )"), ("heapCheck", "true")):
            if not re.search(r"\bw\s*\.\s*%s\s*=\s*%s\s*;" % (field, re.escape(want).replace("\\ ", r"\s*")), w):
                out.append("aiWikiLookup: w.%s is not %s" % (field, want))
        if not re.search(r"\bw\s*\.\s*cancel\s*=\s*&\s*s_cancel\s*;", w):
            out.append("aiWikiLookup: Cancel not honoured (w.cancel = &s_cancel)")
    span = body(ai, "topicQuestions", out, "ai_net.cpp")
    if span and not re.search(r"\bout\s*\[\s*n\+\+\s*\]\s*=\s*q\s*->\s*ctxQ\s*\[\s*i\s*\]\s*;", ai[span[0]:span[1]]):
        out.append("topicQuestions: not the context's questions (q->ctxQ)")
    ua = define(raw_wh, "WIKI_USER_AGENT")
    if not ua or not re.match(r'"[A-Za-z][\w.-]*/[\d.]+ \(https://\S+\)[^"]*"$', ua):
        out.append("wiki_lookup.h: WIKI_USER_AGENT is not Wikimedia's policy form (name/version (contact URL)): %s" % ua)
    cap = define(raw_wh, "WIKI_RESPONSE_CAP")
    if not cap or not re.match(r"\(\s*24u?\s*\*\s*1024u?\s*\)", cap):
        out.append("wiki_lookup.h: WIKI_RESPONSE_CAP is not 24 KB: %s" % cap)
    roots = define(raw_ai, "AI_WIKI_ROOTS_PEM")
    if roots is None or roots.split() != ["ISRG_ROOT_X1_PEM", "ISRG_ROOT_X2_PEM", "ISRG_ROOT_YE_PEM"]:
        out.append("ai_net.cpp: AI_WIKI_ROOTS_PEM is not ISRG X1 + X2 + Root YE (en.wikipedia.org's chain): %s" % roots)
    for name, most in (("AI_WIKI_TOTAL_MS", 20000), ("AI_WIKI_CONNECT_MS", 10000)):
        v = define(raw_ai, name)
        m = re.match(r"(\d+)u?\b", v or "")
        if not m or not 0 < int(m.group(1)) <= most:
            out.append("ai_net.cpp: %s is %s - at most %d ms" % (name, v, most))

    # 8. no tool: in the CODE (comments out, string literals kept - where a JSON key would be)
    for name in ("gemini.cpp", "ai_net.cpp"):
        if re.search(r'url_context|google_search|\\"tools\\"', strip_comments(texts[name])):
            out.append("%s: a tool in the code (url_context / \"tools\" / google_search) - none is ever sent" % name)

    # 9. the chat keeps the plain question
    span = body(ai, "aiLoopTick", out, "ai_net.cpp")
    if span:
        s, e = span
        fold = args_of(ai, s, e, "geminiChatFold")
        if not fold or [p.strip() for p in fold[0][1].split(",")][5] != "s_req->question":
            out.append("aiLoopTick: the chat is not folded with the PLAIN question (s_req->question)")
    for m in re.finditer(r"\bs_work\s*->\s*bg\b", ai):
        fs = [f for f in ("aiWikiLookup", "aiRun", "aiRunChutes", "aiWebSearch")
              if (lambda sp: sp and sp[0] <= m.start() <= sp[1])(function_body(ai, f))]
        if not fs:
            out.append("ai_net.cpp: the background (s_work->bg) is used outside aiWikiLookup/aiRun/aiRunChutes/aiWebSearch")
            break

    # 13. the key folder is write-only on the uploader
    xf, raw_xf = code["app_gbc_xfer.cpp"], texts["app_gbc_xfer.cpp"]
    span = body(xf, "xferKeyPath", out, "app_gbc_xfer.cpp")
    if span and not re.search(r"\breturn\s+filePathIsKeyFile\s*\(\s*path\s*\.\s*c_str\s*\(\s*\)\s*\)\s*!=\s*0\s*;",
                              xf[span[0]:span[1]]):
        out.append("xferKeyPath: not files_paths.h's filePathIsKeyFile (the one rule for what a key file is)")
    held = body(xf, "xferHeld", out, "app_gbc_xfer.cpp")
    if held:
        h = xf[held[0]:held[1]]
        guard = re.search(r"\bif\s*\(\s*xferKeyPath\s*\(\s*path\s*\)\s*\)\s*\{\s*return\s+0\s*;", h)
        op = re.search(r"\bSD\s*\.\s*open\s*\(", h)
        if not guard or not op or guard.start() > op.start():
            out.append("xferHeld: a key file is stat'd - `if (xferKeyPath(path)) { return 0; }` must come before SD.open")
    span = body(xf, "chunkOpenFor", out, "app_gbc_xfer.cpp")
    if span:
        h = xf[span[0]:span[1]]
        guard = re.search(r"\bif\s*\(\s*off\s*!=\s*0\s*&&\s*xferKeyPath\s*\(\s*path\s*\)\s*\)\s*\{\s*\*\s*code\s*=\s*409\s*;"
                          r"[^}]*\breturn\s+false\s*;", h)
        op = re.search(r"\bSD\s*\.\s*open\s*\(", h)
        if not guard or not op or guard.start() > op.start():
            out.append("chunkOpenFor: a key file can be resumed from the card (no 409 for off != 0 before the open)")
    for fn in ("handleChunkGet",):
        span = body(xf, fn, out, "app_gbc_xfer.cpp")
        if span and not re.search(r"\bxferHeld\s*\(", xf[span[0]:span[1]]):
            out.append("%s: the resume question does not go through xferHeld" % fn)
    if len(re.findall(r"\bxferHeld\s*\(\s*name\s*\)", xf)) != 2:
        out.append("app_gbc_xfer.cpp: the two GET /chunk answers (legacy and raw) do not both use xferHeld(name)")
    for m in re.finditer(r"\bSD\s*\.\s*open\s*\(", xf):
        po = m.end() - 1
        pc = match_close(xf, po)
        a = xf[po:pc] if pc > 0 else ""
        if re.search(r"\bFILE_(?:WRITE|APPEND)\b", a):
            continue                                   # a write: the uploader's job
        if held and held[0] <= m.start() <= held[1]:
            continue                                   # xferHeld's stat, behind the key guard
        if raw_xf[po:pc + 1] == '("/health.log")':
            continue                                   # /log: that one file only
        out.append("app_gbc_xfer.cpp:%d: SD.open%s opens a file to READ (only /health.log and xferHeld may)"
                   % (xf.count("\n", 0, m.start()) + 1, re.sub(r"\s+", " ", raw_xf[po:pc + 1])))
    if re.search(r"\bopenNextFile\s*\(|\bSD\s*\.\s*exists\s*\(|\bopendir\s*\(", xf):
        out.append("app_gbc_xfer.cpp: the uploader lists or probes the card (openNextFile / SD.exists / opendir)")
    check_chutes(texts, code, out)
    check_review(texts, code, out)
    check_topic(code, out)
    check_web(texts, code, out)
    return out


def check_chutes(texts, code, out):
    """Contracts 14-17 (D-175): Chutes' wire, roots, loop, keys and choice."""
    ai, raw_ai = code["ai_net.cpp"], texts["ai_net.cpp"]
    ai_c = blank_comments(raw_ai)                       # code with its string literals, at strip_code's offsets
    raw_ch = texts["chutes.h"]

    # 14. the Bearer goes only to llm.chutes.ai
    if define(raw_ch, "CHUTES_HOST") != '"llm.chutes.ai"':
        out.append("chutes.h: CHUTES_HOST is not \"llm.chutes.ai\": %s" % define(raw_ch, "CHUTES_HOST"))
    if define(raw_ch, "CHUTES_URL") != '"https://" CHUTES_HOST "/v1/chat/completions"':
        out.append("chutes.h: CHUTES_URL is not https://CHUTES_HOST/v1/chat/completions: %s" % define(raw_ch, "CHUTES_URL"))
    span = body(ai, "postOnceChutes", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        rw = ai_c[s:e]
        for field, want in (("host", "CHUTES_HOST"), ("url", "CHUTES_URL"), ("rootsPem", "AI_CHUTES_ROOTS_PEM"),
                            ("timeoutMs", "p->waitMs"), ("waitSliceMs", "AI_SLICE_MS"), ("stopFn", "tileFetchCallRecent"),
                            ("heapCheck", "true"), ("sink", "netWorkerSink ( )"), ("cap", "AI_SINK_CAP"),
                            ("hdrValue[1]", "s_work->auth")):
            f = re.escape(field).replace("\\[", r"\s*\[\s*").replace("\\]", r"\s*\]")
            v = re.escape(want).replace("\\ ", r"\s*").replace("\\-\\>", r"\s*->\s*").replace("\\->", r"\s*->\s*")
            if not re.search(r"\bq\s*\.\s*%s\s*=\s*%s\s*;" % (f, v), w):
                out.append("postOnceChutes: q.%s is not %s" % (field, want))
        if not re.search(r'\bq\s*\.\s*hdrName\s*\[\s*1\s*\]\s*=\s*"Authorization"\s*;', rw):
            out.append("postOnceChutes: the key is not sent as the Authorization header")
        if not re.search(r"\bq\s*\.\s*cancel\s*=\s*&\s*s_cancel\s*;", w):
            out.append("postOnceChutes: Cancel not honoured (q.cancel = &s_cancel)")
        auth = re.search(r"\bchutesAuthHeader\s*\(\s*s_req\s*->\s*chutes\s*\.\s*key\s*,\s*s_work\s*->\s*auth\s*,", w)
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        wipe = re.search(r"\bmemset\s*\(\s*s_work\s*->\s*auth\s*,\s*0\s*,", w)
        if not auth or len(net) != 1 or auth.start() > net[0].start():
            out.append("postOnceChutes: the header is not built from the CHUTES key (chutesAuthHeader) before its one netHttps")
        elif not wipe or wipe.start() < net[0].start() or re.search(r"\breturn\b", w[net[0].start():wipe.start()]):
            out.append("postOnceChutes: the header is not wiped right after netHttps returns")
        if re.search(r"\bcfg\s*\.\s*key\b", w) or "x-goog-api-key" in rw:
            out.append("postOnceChutes: Gemini's key in a Chutes request")
    span = body(ai, "postOnce", out, "ai_net.cpp")
    if span and (re.search(r"\bchutes\s*\.\s*key\b|\bauth\b", ai[span[0]:span[1]]) or
                 "Authorization" in ai_c[span[0]:span[1]] or "Bearer" in ai_c[span[0]:span[1]]):
        out.append("postOnce: Gemini's request carries the Chutes key or an Authorization header")
    pc = function_body(ai, "postOnceChutes")
    ws = function_body(ai, "aiWebSearch")              # D-176: Tavily's Bearer, contract 22
    for word in ("Authorization", "Bearer", "chutesAuthHeader"):
        for m in re.finditer(re.escape(word), ai_c):
            inside = pc and pc[0] <= m.start() <= pc[1]
            if word == "Authorization" and ws and ws[0] <= m.start() <= ws[1]:
                inside = True
            if not inside:
                out.append("ai_net.cpp:%d: %s outside postOnceChutes%s - the Bearer goes to llm.chutes.ai only"
                           % (ai_c.count("\n", 0, m.start()) + 1, word, " / aiWebSearch" if word == "Authorization" else ""))
                break
    if re.search(r"\bsetFollowRedirects\s*\(", code["https_worker.cpp"] + ai):
        out.append("https_worker.cpp/ai_net.cpp: redirects followed - the Bearer would go where a 30x says")

    # 15. the roots
    roots = define(raw_ai, "AI_CHUTES_ROOTS_PEM")
    if roots is None or roots.split() != ["GODADDY_ROOT_G2_PEM", "ISRG_ROOT_X1_PEM", "ISRG_ROOT_X2_PEM",
                                          "ISRG_ROOT_YR_PEM", "ISRG_ROOT_YE_PEM", "GTS_ROOT_R1_PEM", "GTS_ROOT_R4_PEM"]:
        out.append("ai_net.cpp: AI_CHUTES_ROOTS_PEM is not GoDaddy G2 + ISRG X1 + X2 + YR + YE + GTS R1 + R4: %s" % roots)
    if not re.search(r"static\s+const\s+char\s+AI_ROOTS_PEM\s*\[\s*\]\s*=\s*GTS_ROOT_R1_PEM\s+GTS_ROOT_R4_PEM\s*;", raw_ai):
        out.append("ai_net.cpp: Gemini's AI_ROOTS_PEM is not GTS R1 + R4")

    # 16. aiRunChutes
    span = body(ai, "aiRunChutes", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        lp = re.search(r"\bfor\s*\(\s*;\s*;\s*\)\s*\{", w)
        sysc = args_of(ai, s, e, "chutesSystemText")
        builds = args_of(ai, s, e, "chutesBuildRequest")
        if not sysc or not sysc[0][1].startswith("utcNow()"):
            out.append("aiRunChutes: chutesSystemText is not given utcNow() (the trusted clock)")
        elif not lp or sysc[0][0] < s + lp.start() or (builds and sysc[0][0] > builds[0][0]):
            out.append("aiRunChutes: the system text is not built per request, before the body")
        if len(builds) != 2:
            out.append("aiRunChutes: chutesBuildRequest called %d times (the length, then the body)" % len(builds))
        for _, a in builds:
            parts = [x.strip() for x in a.split(",")]
            if len(parts) < 7 or parts[3:6] != ["q->question", "s_work->bg", "s_work->sys"]:
                out.append("aiRunChutes: chutesBuildRequest(%s) - not the plain question, the background and the text" % a)
        sd = re.search(r"\bseed\s*=\s*chutesSeed\s*\(\s*esp_random\s*\(\s*\)\s*,\s*seed\s*\)\s*;", w)
        if not sd or not lp or sd.start() < lp.start() or (builds and s + sd.start() > builds[0][0]):
            out.append("aiRunChutes: no fresh seed per attempt (seed = chutesSeed(esp_random(), seed) in the loop)")
        post = re.search(r"\bpostOnceChutes\s*\(", w)
        nx = args_of(ai, s, e, "chutesLadderNext")
        cn = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*s_cancel\s*\)\s*\{\s*r\s*->\s*cancelled\s*=\s*true\s*;\s*break\s*;", w)
        call = re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*!\s*s_cancel\s*&&\s*tileFetchCallRecent\s*\(\s*\)\s*\)", w)
        if not post or not cn or not call or cn.start() < post.start() or call.start() < post.start() or \
                (nx and (s + cn.start() > nx[0][0] or s + call.start() > nx[0][0])):
            out.append("aiRunChutes: Cancel and a call are not ended after the request, before the ladder")
        if len(nx) != 1 or [x.strip() for x in nx[0][1].split(",")][1:3] != ["code", "net"]:
            out.append("aiRunChutes: chutesLadderNext is not given the attempt's code and net")
        at = args_of(ai, s, e, "chutesAnswerShown")
        sc = re.search(r"\bgeminiScrub\s*\(\s*r\s*->\s*answer\s*,\s*sizeof\s*\(\s*r\s*->\s*answer\s*\)\s*,\s*q\s*->\s*chutes\s*\.\s*key\s*\)", w)
        if not at or not sc or s + sc.start() < at[0][0]:
            out.append("aiRunChutes: the answer is not scrubbed with the Chutes key after chutesAnswerShown")
        if at and "s_work->src[0] ? s_work->src : NULL" not in at[0][1]:
            out.append("aiRunChutes: the sources line is not s_work->src's own (D-176): %s" % at[0][1])
        if at and [x.strip() for x in at[0][1].split(",")][:3] != ["rep", "l.cur", "s_work->text"]:
            out.append("aiRunChutes: chutesAnswerShown is not given the preset that answered (rep, l.cur, s_work->text, "
                       "...): %s" % at[0][1])
        # the WiPhone's review (2026-10-19): built once, after the parse, BEFORE the ladder reads the reply
        prs = args_of(ai, s, e, "chutesParseReply")
        if len(at) != 1 or args_of(ai, s, e, "chutesAnswerText") or not prs or not nx or \
                not prs[0][0] < at[0][0] < nx[0][0]:
            out.append("aiRunChutes: the answer is not built once by chutesAnswerShown between the parse and the "
                       "ladder - the ladder would read an emoji alone as an answer ('sent an empty answer', no retry)")
        if not re.search(r"\bif\s*\(\s*rep\s*->\s*kind\s*==\s*CHUTES_REPLY_TEXT\s*&&\s*r\s*->\s*answer\s*\[\s*0\s*\]\s*\)\s*\{\s*"
                         r"r\s*->\s*ok\s*=\s*true\s*;", w):
            out.append("aiRunChutes: a DONE is not an answer exactly when chutesAnswerShown left a TEXT with words")
        if re.search(r"\baiWikiLookup\s*\(", w):
            out.append("aiRunChutes: a lookup of its own - it would be made per Chutes question twice")
        if not re.search(r"\bif\s*\(\s*code\s*<=\s*0\s*&&\s*net\s*==\s*GEM_NET_NOMEM\s*\)\s*\{\s*lowMemText\s*\(\s*r\s*->\s*fail\s*,", w):
            out.append("aiRunChutes: a NOMEM failure does not say 'reboot first' (lowMemText)")
        pr = args_of(ai, s, e, "chutesParseReply")
        if len(pr) != 1 or pr[0][1] != "l.cur, netWorkerSink(), got, s_work->text, sizeof(s_work->text), rep":
            out.append("aiRunChutes: the reply is not parsed as the attempt's preset's (chutesParseReply(l.cur, ...)): %s"
                       % (pr[0][1] if pr else "no call"))
        # review 2026-10-18: "Chutes has answered" on DONE; the gone presets kept after the loop
        if not re.search(r"\bif\s*\(\s*step\s*==\s*GEM_STEP_DONE\s*\)\s*\{\s*s_m\s*->\s*chutesAnswered\s*=\s*true\s*;", w):
            out.append("aiRunChutes: a DONE does not set s_m->chutesAnswered (a later 429 would say 'check the key')")
        lpe = match_close(w, lp.end() - 1) if lp else -1
        gm = re.search(r"\bs_m\s*->\s*chutesGone\s*=\s*l\s*\.\s*goneMask\s*;", w)
        if not gm or lpe < 0 or gm.start() < lpe:
            out.append("aiRunChutes: the gone presets are not kept after the loop (s_m->chutesGone = l.goneMask)")
    span = body(ai, "aiRun", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        rc = list(re.finditer(r"\baiRunChutes\s*\(", w))
        wl = re.search(r"\baiWikiLookup\s*\(", w)
        if len(rc) != 1 or not wl or rc[0].start() < wl.start():
            out.append("aiRun: aiRunChutes is not called once, after the one lookup")
        st = re.search(r"\bchutesLadderStart\s*\(", w)
        rt = re.search(r"\bs_work\s*->\s*cl\s*\.\s*retryTimeouts\s*=\s*q\s*->\s*retryTimeouts\s*;", w)
        if not st or not rt or rt.start() < st.start() or (wl and rt.start() > wl.start()):
            out.append("aiRun: Chutes' ladder is not given the retry setting right after chutesLadderStart")
        ls = args_of(ai, s, e, "chutesLadderStart")
        if len(ls) != 1 or [x.strip() for x in ls[0][1].split(",")] != \
                ["&s_work->cl", "q->choice", "s_m->chutesGone", "!s_m->chutesAnswered", "millis()"]:
            out.append("aiRun: chutesLadderStart is not given the boot's gone presets and !chutesAnswered: %s"
                       % (ls[0][1] if ls else "no call"))
        # review 2026-10-18: never Gemini in place of Chutes - the branch returns, whatever happened
        disp = re.search(r"\bif\s*\(\s*chutes\s*\)\s*\{\s*aiRunChutes\s*\(\s*q\s*,\s*r\s*,\s*ctx\s*\)\s*;\s*"
                         r"r\s*->\s*ms\s*=\s*millis\s*\(\s*\)\s*-\s*t0\s*;\s*return\s*;\s*\}", w)
        post = re.search(r"\bpostOnce\s*\(", w)
        if not disp or not post or disp.start() > post.start():
            out.append("aiRun: the Chutes branch does not RETURN before Gemini's loop - a Chutes question would "
                       "go on to Google (a key-less POST, Google's words over Chutes')")

    # 17. no key in a log, a screen or the console; the choice copied at ask time
    secret = re.compile(r"(?:\b(?:cfg|chutes|web|s_cfg|s_ccfg|s_wcfg)\s*(?:\.|->)\s*key\b|\bs_work\s*->\s*(?:auth|webAuth)\b|\bbuf\b)")
    for fn in ("log_e", "log_w", "log_i", "log_d", "log_v", "snprintf", "emit", "progSet", "printf"):
        for pos, a in args_of(ai, 0, len(ai), fn):
            if secret.search(a):
                out.append("ai_net.cpp:%d: %s(%s) - a key (or the file it was read from) in a printed or drawn text"
                           % (ai.count("\n", 0, pos) + 1, fn, a[:60]))
    hh = code["ai_net.h"]
    for st_name in ("AiKeyInfo", "AiChutesKey", "AiWebKey"):
        m = re.search(r"typedef\s+struct\s*\{([^}]*)\}\s*%s\s*;" % st_name, hh)
        if not m:
            out.append("ai_net.h: %s not found" % st_name)
        elif re.search(r"\bkey\s*\[|\bGeminiConfig\b|\bChutesConfig\b|\bWebConfig\b", m.group(1)):
            out.append("ai_net.h: %s carries a key - the app and the console would hold it" % st_name)
    for name in ("app_ai.cpp", "serial_cmd.cpp"):
        if re.search(r"\b(?:gemini|chutes|web)ParseConfig\s*\(|\b(?:chutes|web)AuthHeader\s*\(|\b(?:cfg|chutes|web)\s*\.\s*key\b",
                     code[name]):
            out.append("%s: reads a key file or a key itself - only ai_net.cpp may" % name)
    sc = code["serial_cmd.cpp"]
    m = re.search(r"if\s*\(\s*!\s*strcasecmp\s*\(\s*p\s*,\s*[^)]*\)\s*\|\|\s*!\s*strncasecmp\s*\(\s*p\s*,\s*[^)]*,\s*6\s*\)\s*\)\s*\{", sc)
    if not m:
        out.append("serial_cmd.cpp: the `ai model` branch not found")
    else:
        bs = m.end() - 1
        be = match_close(sc, bs)
        for pos, a in args_of(sc, bs, be, "say"):
            if re.search(r"\bid\b|\bp\b", a):
                out.append("serial_cmd.cpp: `ai model` echoes what was typed (a key pasted there would reach the log)")
                break
    span = body(ai, "aiAsk", out, "ai_net.cpp")
    if span:
        s, e = span
        w = ai[s:e]
        ch = re.search(r"\bconst\s+int\s+choice\s*=\s*aiModelChoice\s*\(\s*\)\s*;", w)
        cp = re.search(r"\bs_req\s*->\s*choice\s*=\s*choice\s*;", w)
        if not ch or not cp or cp.start() < ch.start():
            out.append("aiAsk: the choice is not copied into the question (s_req->choice = aiModelChoice())")
        if not re.search(r"\bif\s*\(\s*chutes\s*\)\s*\{[^}]*\bmemset\s*\(\s*&\s*s_req\s*->\s*cfg\s*,\s*0\s*,[^}]*"
                         r"\bmemcpy\s*\(\s*&\s*s_req\s*->\s*chutes\s*,\s*s_ccfg\s*,[^}]*\}\s*else\s*\{[^}]*"
                         r"\bmemcpy\s*\(\s*&\s*s_req\s*->\s*cfg\s*,\s*s_cfg\s*,[^}]*\bmemset\s*\(\s*&\s*s_req\s*->\s*chutes\s*,\s*0\s*,", w):
            out.append("aiAsk: a question does not carry ONLY its own provider's key")
    for fn, wipes in (("aiJob", (r"s_req\s*->\s*chutes\s*\.\s*key", r"s_work\s*->\s*auth")),
                      ("aiLoopTick", (r"s_req\s*->\s*chutes\s*\.\s*key",))):
        span = body(ai, fn, out, "ai_net.cpp")
        if span:
            for wrx in wipes:
                if not re.search(r"\bmemset\s*\(\s*%s\s*,\s*0\s*," % wrx, ai[span[0]:span[1]]):
                    out.append("%s: the Chutes key copy (%s) is not wiped" % (fn, wrx.replace("\\s*", "")))
    span = body(ai, "aiModelChoice", out, "ai_net.cpp")
    if span and not re.search(r"\breturn\s+chutesChoiceResolve\s*\(\s*s_m\s*->\s*modelStored\s*,\s*s_m\s*->\s*ckey\s*\.\s*usable\s*,"
                              r"\s*s_m\s*->\s*ckey\s*\.\s*preset\s*\)\s*;", ai[span[0]:span[1]]):
        out.append("aiModelChoice: not chutesChoiceResolve(NVS, the Chutes key read, its model=) - the default rule")
    span = body(ai, "modelLoad", out, "ai_net.cpp")
    if span:
        w = ai[span[0]:span[1]]
        ik = re.search(r"\bp\s*\.\s*isKey\s*\(\s*AI_PREFS_MODEL\s*\)", w)
        gs = re.search(r"\bp\s*\.\s*getString\s*\(\s*AI_PREFS_MODEL\s*,", w)
        if not ik or not gs or gs.start() < ik.start():
            out.append("modelLoad: getString without isKey first (an absent ai_model logs an error line)")
    pm = (define(raw_ai, "AI_PREFS_MODEL") or "").split()
    if not pm or not re.fullmatch(r'"[a-z_]{1,15}"', pm[0]) or pm[0] != '"ai_model"':
        out.append("ai_net.cpp: AI_PREFS_MODEL is not \"ai_model\" (an NVS key: 15 characters at most)")
    span = body(ai, "aiSetModelChoice", out, "ai_net.cpp")
    if span:
        w = ai[span[0]:span[1]]
        if not re.search(r"\bchoice\s*!=\s*CHUTES_GEMINI\s*&&\s*!\s*chutesPreset\s*\(\s*choice\s*\)", w) or \
                not re.search(r"\bstrlcpy\s*\(\s*s_m\s*->\s*modelStored\s*,\s*chutesChoiceId\s*\(\s*choice\s*\)", w):
            out.append("aiSetModelChoice: stores something that is not an allowlisted id")
    if define(texts["ai_net.h"], "AI_CHUTES_FILE") is None or \
            not define(texts["ai_net.h"], "AI_CHUTES_FILE").startswith('"/API Keys/chutes.txt"'):
        out.append("ai_net.h: AI_CHUTES_FILE is not \"/API Keys/chutes.txt\"")
    span = body(ai, "chutesKeyReload", out, "ai_net.cpp")
    if span:
        w = ai_c[span[0]:span[1]]
        if re.search(r'"/[^"]*chutes\.txt"|AI_KEY_FILE', w) or len(re.findall(r"\bAI_CHUTES_FILE\b", w)) < 2:
            out.append("chutesKeyReload: chutes.txt is read from somewhere other than AI_CHUTES_FILE")
    check_secrets(texts, code, out)
    check_latch(code, out)


KEYREAD = re.compile(r"(?:\.|->)\s*key\b(?!\s*(?:\.|->))")
AUTH = re.compile(r"\bs_work\s*->\s*auth\b")
WEBAUTH = re.compile(r"\bs_work\s*->\s*webAuth\b")
SINK = re.compile(r"\bnetWorkerSink\s*\(\s*\)")
RAWTEXT = re.compile(r"\bs_work\s*->\s*text\b")


def check_secrets(texts, code, out):
    """Contract 17, widened (review 2026-10-18): every read of a key, the header, the key file's
    buffer and the parse's local, the raw body and the raw text is an allowlisted one."""
    ai, ai_c = code["ai_net.cpp"], blank_comments(texts["ai_net.cpp"])

    def aikeyinfo(m):                                    # s_m->key is AiKeyInfo (no key: ai_net.h, pinned)
        return re.search(r"\bs_m\s*$", ai[max(0, m.start() - 16):m.start()]) is not None
    uses_allowed(ai, KEYREAD, (("memset", 0), ("geminiWipe", 0), ("sizeof", None), ("chutesAuthHeader", 0),
                               ("webAuthHeader", 0), ("geminiScrub", 2), ("chutesFailText", 4), ("geminiFailText", 4)),
                 "ai_net.cpp", "a key read outside the wipes, the header, the scrub and the failure words", out,
                 statements=(r"q\.hdrValue\[1\] = s_req->cfg\.key",), skip=aikeyinfo)
    uses_allowed(ai, AUTH, (("memset", 0), ("geminiWipe", 0), ("sizeof", None), ("chutesAuthHeader", 1)),
                 "ai_net.cpp", "the Bearer header outside its build, its one send and its wipes", out,
                 statements=(r"q\.hdrValue\[1\] = s_work->auth", r"chutesAuthHeader\(.*\) == 0 \|\| !s_work->auth\[0\]"))
    # D-176: Tavily's header - built once, sent once, wiped (contract 22 says where)
    uses_allowed(ai, WEBAUTH, (("memset", 0), ("geminiWipe", 0), ("sizeof", None), ("webAuthHeader", 1)),
                 "ai_net.cpp", "the Tavily Bearer outside its build, its one send and its wipes", out,
                 statements=(r"w\.hdrValue\[1\] = s_work->webAuth", r"bn == 0 \|\| .* \|\| !s_work->webAuth\[0\]"))
    uses_allowed(ai, SINK, (("geminiParseReply", 0), ("chutesParseReply", 1), ("wikiParse", 0)),
                 "ai_net.cpp", "the raw body (a 402 names a wallet) goes only into the parsers", out,
                 statements=(r"[qw]\.sink = netWorkerSink\(\)",))
    uses_allowed(ai, RAWTEXT, (("geminiParseReply", 2), ("chutesParseReply", 3), ("sizeof", None),
                               ("geminiAnswerText", 1), ("chutesAnswerShown", 2)),
                 "ai_net.cpp", "the answer's raw text goes only to the parser and the answer builder", out)
    for fn, parse in (("chutesKeyReload", "chutesParseConfig"), ("aiKeyReload", "geminiParseConfig"),
                      ("webKeyReload", "webParseConfig")):
        span = body(ai, fn, out, "ai_net.cpp")
        if not span:
            continue
        uses_allowed(ai, re.compile(r"\bbuf\b"), (("readSmall", 1), (parse, 0), ("geminiWipe", 0), ("free", 0)),
                     "ai_net.cpp", "%s's read of the key file outside the read, the parse, the wipe" % fn, out,
                     span[0], span[1], statements=(r"char\* buf = .*", r"const int n = buf \? .*", r"buf"))
        uses_allowed(ai, re.compile(r"\bc\b"), ((parse, 2), ("chutesConfigLine", 0), ("webConfigLine", 0), ("keyInfoFrom", 0),
                                                   ("memcpy", 1),
                                                   ("geminiWipe", 0), ("free", 0), ("sizeof", None)),
                     "ai_net.cpp", "%s's parse (it holds the key) outside the allowed uses" % fn, out,
                     span[0], span[1], statements=(r"(?:Chutes|Gemini|Web)Config\* c = .*", r"!c"),
                     skip=lambda m: re.match(r"\s*->\s*(?!key\b)\w+", ai[m.end():m.end() + 40]) is not None)
    # Key info's lines: a *ConfigLine or a literal, never anything else
    for fn in ("strlcpy", "strncpy", "strcpy", "memcpy", "snprintf", "sprintf", "strlcat", "strcat"):
        for m in re.finditer(r"\b%s\s*\(" % fn, ai):
            c = call_at(ai, m.end())
            if not c or not c[1]:
                continue
            dst = re.sub(r"\s+", "", ai[c[1][0][0]:c[1][0][1]])
            if dst in ("s_m->ckey.line", "s_m->key.line", "s_m->wkey.line"):
                src = ai_c[c[1][1][0]:c[1][1][1]].strip() if len(c[1]) > 1 else ""
                if not re.fullmatch(r'"(?:[^"\\]|\\.)*"', src):
                    out.append("ai_net.cpp:%d: %s(%s, %s) - Key info's line is written only by its *ConfigLine or a literal"
                               % (line_of(ai, m.start()), fn, dst, src[:40]))
    # gemini.txt through Gemini's key rule (the cpk_ refusal); the bare reader is chutes.cpp's alone
    for name in ("ai_net.cpp", "app_ai.cpp", "serial_cmd.cpp"):
        if re.search(r"\bgeminiReadKeyFile\s*\(", code[name]):
            out.append("%s: geminiReadKeyFile - past Gemini's key rule (a Chutes key would go to Google)" % name)
    span = function_body(ai, "aiKeyReload")
    if span and not re.search(r"\bgeminiParseConfig\s*\(\s*buf\s*,", ai[span[0]:span[1]]):
        out.append("aiKeyReload: gemini.txt is not parsed by geminiParseConfig (the cpk_ refusal)")
    wc = code["websearch.cpp"]
    span = function_body(wc, "webParseConfig")
    if not span or not re.search(r"\bgeminiReadKeyFile\s*\(\s*text\s*,\s*len\s*,\s*g\s*\)", wc[span[0]:span[1]]) or \
            not re.search(r"\bstrncmp\s*\(\s*g\s*->\s*key\s*,\s*WEB_KEY_PREFIX\s*,", wc[span[0]:span[1]]):
        out.append("webParseConfig: not geminiReadKeyFile's rules with the tvly- rule (WEB_KEY_PREFIX)")
    span = function_body(wc, "webAuthHeader")
    if not span or not re.search(r"\bstrncmp\s*\(\s*key\s*,\s*WEB_KEY_PREFIX\s*,", wc[span[0]:span[1]]):
        out.append("webAuthHeader: a key that is not tvly-... can become a Bearer")
    gc = code["gemini.cpp"]
    span = function_body(gc, "geminiParseConfig")
    if not span or not re.search(r"\bstrncmp\s*\(\s*out\s*->\s*key\s*,\s*GEM_TAVILY_KEY_PREFIX\s*,", gc[span[0]:span[1]]):
        out.append("geminiParseConfig: a tvly- key is not refused (it would go to Google as x-goog-api-key)")
    if (define(texts["gemini.h"], "GEM_TAVILY_KEY_PREFIX") or "").split()[:1] != ['"tvly-"'] or \
            (define(texts["websearch.h"], "WEB_KEY_PREFIX") or "").split()[:1] != ["GEM_TAVILY_KEY_PREFIX"]:
        out.append("gemini.h / websearch.h: the Tavily prefix is not one \"tvly-\" (GEM_TAVILY_KEY_PREFIX, WEB_KEY_PREFIX)")
    cc = code["chutes.cpp"]
    span = function_body(cc, "chutesParseConfig")
    if span and not re.search(r"\bgeminiReadKeyFile\s*\(\s*text\s*,\s*len\s*,\s*g\s*\)", cc[span[0]:span[1]]):
        out.append("chutesParseConfig: not geminiReadKeyFile (geminiParseConfig refuses every cpk_ key)")


def check_latch(code, out):
    """Contract 17's device inputs of the default rule (review 2026-10-18)."""
    ai = code["ai_net.cpp"]
    for fn, cond, good, cfg, ki in (("chutesKeyReload", r"!\s*there", "s_cHaveGood", "s_ccfg", "ckey"),
                                    ("aiKeyReload", r"!\s*path", "s_haveGood", "s_cfg", "key"),
                                    ("webKeyReload", r"!\s*there", "s_wHaveGood", "s_wcfg", "wkey")):
        span = body(ai, fn, out, "ai_net.cpp")
        if not span:
            continue
        w = ai[span[0]:span[1]]
        miss = {"s_cHaveGood": "s_cKeyMisses", "s_haveGood": "s_keyMisses", "s_wHaveGood": "s_wKeyMisses"}[good]
        if not re.search(r"\bif\s*\(\s*%s\s*&&\s*%s\s*&&\s*%s\s*<\s*2\s*\)\s*\{" % (cond, good, miss), w):
            out.append("%s: a missed read keeps a key without a good read before it (%s && %s && %s < 2)"
                       % (fn, cond.replace("\\s*", ""), good, miss))
        if not re.search(r"\bif\s*\(\s*n\s*<\s*0\s*\)\s*\{[^}]*\bs_m\s*->\s*%s\s*\.\s*usable\s*=\s*%s\s*;" % (ki, good), w):
            out.append("%s: an unreadable file is usable without a good read (s_m->%s.usable = %s)" % (fn, ki, good))
        if not re.search(r"\}\s*else\s*\{\s*memset\s*\(\s*%s\s*,\s*0\s*,\s*sizeof\s*\(\s*\*\s*%s\s*\)\s*\)\s*;"
                         r"(?:\s*%s\s*->\s*preset\s*=\s*-\s*1\s*;)?\s*%s\s*=\s*false\s*;" % (cfg, cfg, cfg, good), w):
            out.append("%s: a refused file keeps the key it replaces (memset(%s) and %s = false)" % (fn, cfg, good))
    span = body(ai, "aiChoiceUsable", out, "ai_net.cpp")
    if span and not re.search(r"\breturn\s+chutesPreset\s*\(\s*choice\s*\)\s*\?\s*s_m\s*->\s*ckey\s*\.\s*usable\s*:"
                              r"\s*s_m\s*->\s*key\s*\.\s*usable\s*;", ai[span[0]:span[1]]):
        out.append("aiChoiceUsable: not each provider's own key (chutesPreset(choice) ? ckey.usable : key.usable)")
    span = body(ai, "aiAsk", out, "ai_net.cpp")
    if span and not re.search(r"\bconst\s+bool\s+usable\s*=\s*chutes\s*\?\s*\(\s*s_m\s*->\s*ckey\s*\.\s*usable\s*&&\s*s_ccfg\s*&&"
                              r"\s*s_cHaveGood\s*\)\s*:\s*s_m\s*->\s*key\s*\.\s*usable\s*;", ai[span[0]:span[1]]):
        out.append("aiAsk: a Chutes question is not gated on a GOOD chutes.txt read (ckey.usable && s_ccfg && s_cHaveGood)")


def check_review(texts, code, out):
    """Contracts 18-20 (the D-175 review, 2026-10-18)."""
    ai = code["ai_net.cpp"]
    # 18. the wipes the compiler keeps
    for m in re.finditer(r"\bmemset\s*\(\s*(\w+)\s*,[^;]*\)\s*;\s*free\s*\(\s*\1\s*\)", ai):
        out.append("ai_net.cpp:%d: memset(%s) right before free(%s) - a dead store gcc drops: geminiWipe"
                   % (line_of(ai, m.start()), m.group(1), m.group(1)))
    for fn, names in (("chutesKeyReload", (("c", r"sizeof\s*\(\s*\*\s*c\s*\)"), ("buf", r"AI_KEY_READ_MAX"))),
                      ("aiKeyReload", (("c", r"sizeof\s*\(\s*\*\s*c\s*\)"), ("buf", r"AI_KEY_READ_MAX"))),
                      ("webKeyReload", (("c", r"sizeof\s*\(\s*\*\s*c\s*\)"), ("buf", r"AI_KEY_READ_MAX"))),
                      ("aiRunChutes", (("rep", r"sizeof\s*\(\s*\*\s*rep\s*\)"),)),
                      ("aiRun", (("rep", r"sizeof\s*\(\s*\*\s*rep\s*\)"),))):
        span = body(ai, fn, out, "ai_net.cpp")
        if not span:
            continue
        w = ai[span[0]:span[1]]
        for name, size in names:
            frees = len(re.findall(r"\bfree\s*\(\s*%s\s*\)" % name, w))
            wiped = len(re.findall(r"\bgeminiWipe\s*\(\s*%s\s*,\s*%s\s*\)\s*;\s*free\s*\(\s*%s\s*\)" % (name, size, name), w))
            if fn == "aiRun" and name == "rep":
                frees = len(re.findall(r"\bfree\s*\(\s*rep\s*\)\s*;\s*r\s*->\s*ms", w))   # the one after the loop
                wiped = min(wiped, frees)
            if frees == 0 or wiped != frees:
                out.append("%s: free(%s) without geminiWipe(%s, ...) right before it (%d of %d) - what it held stays "
                           "in freed PSRAM" % (fn, name, name, wiped, frees))
    gc = code["gemini.cpp"]
    span = function_body(gc, "geminiWipe")
    if not span or not re.search(r"\bvolatile\s+unsigned\s+char\s*\*", gc[span[0]:span[1]]) or \
            re.search(r"\bmemset\s*\(", gc[span[0]:span[1]]):
        out.append("gemini.cpp: geminiWipe is not volatile stores (a memset the compiler may drop)")

    # 19. the screens never trap
    app = code["app_ai.cpp"]
    pe = function_body(app, "AiApp::processEvent")
    if not pe:
        out.append("app_ai.cpp: AiApp::processEvent not found")
    else:
        w = app[pe[0]:pe[1]]
        ki = re.search(r"\bcase\s+AI_KEYINFO\s*:", w)
        kx = re.search(r"\bcase\s+AI_XFER\s*:", w)
        block = w[ki.end():kx.start()] if ki and kx and kx.start() > ki.end() else ""
        back = re.search(r"\bif\s*\(\s*LOGIC_BUTTON_BACK\s*\(\s*event\s*\)\s*\)\s*\{", block)
        bb = block[back.end():match_close(block, back.end() - 1)] if back else ""
        info = re.search(r"\baiChutesKeyInfo\s*\(\s*&\s*ckey\s*\)\s*;", bb)
        gate = re.search(r"\bif\s*\(\s*!\s*key\s*\.\s*usable\s*&&\s*!\s*ckey\s*\.\s*usable\s*&&\s*\(\s*!\s*aiChat\s*\(\s*\)\s*\|\|"
                         r"\s*aiChat\s*\(\s*\)\s*->\s*n\s*==\s*0\s*\)\s*\)\s*\{\s*return\s+EXIT_APP\s*;", bb)
        if not gate or not info or info.start() > gate.start() or re.search(r"\bchoiceUsable\s*\(", bb):
            out.append("app_ai.cpp: Key info's Back leaves the app while a key is there (it must ask !key.usable && "
                       "!ckey.usable, freshly read) - the model in use keyless traps Menu > Model")
        if not re.search(r"\bif\s*\(\s*k\s*==\s*ROW_MODEL\s*\)\s*\{[^}]*\benterState\s*\(\s*AI_MODELS\s*\)", block):
            out.append("app_ai.cpp: Key info's Model row does not open the picker (k == ROW_MODEL -> AI_MODELS)")
        mo = re.search(r"\bcase\s+AI_MODELS\s*:", w)
        mk = re.search(r"\bcase\s+AI_KEYINFO\s*:", w)
        mb = w[mo.end():mk.start()] if mo and mk and mk.start() > mo.end() else ""
        if not re.search(r"\bif\s*\(\s*c\s*!=\s*aiModelChoice\s*\(\s*\)\s*\)\s*\{\s*aiSetModelChoice\s*\(\s*c\s*\)\s*;\s*\}", mb) or \
                len(re.findall(r"\baiSetModelChoice\s*\(", mb)) != 1:
            out.append("app_ai.cpp: OK on the picker's row in use stores it (it must store only c != aiModelChoice())")
    span = function_body(app, "AiApp::buildKeyInfo")
    if not span or not re.search(r"\bconst\s+bool\s+modelRow\s*=\s*!\s*aiChoiceUsable\s*\(\s*choice\s*\)\s*&&\s*\(\s*key\s*\.\s*usable"
                                 r"\s*\|\|\s*ckey\s*\.\s*usable\s*\)\s*;", app[span[0]:span[1]]) or \
            not re.search(r"\bif\s*\(\s*modelRow\s*\)\s*\{[^}]*\bmenu\s*->\s*addOption\s*\(\s*line\s*,\s*ROW_MODEL\s*\)", app[span[0]:span[1]]):
        out.append("app_ai.cpp: Key info has no Model row when the model in use is keyless and the other key is there")
    span = function_body(app, "AiApp::drawXfer")
    if not span or not re.search(r"\bconst\s+bool\s+found\s*=\s*choiceUsable\s*\(\s*\)\s*;", app[span[0]:span[1]]) or \
            re.search(r"\bkey\s*\.\s*usable\s*\|\|\s*ckey\s*\.\s*usable", app[span[0]:span[1]]):
        out.append("app_ai.cpp: the uploader's green line is not the model in use's key (found = choiceUsable())")

    # 20. key files are never cloned
    fp = texts["files_paths.h"]
    span = function_body(strip_code(fp), "filePathIsKeyFile")
    names = re.findall(r'\bstrncasecmp\s*\(\s*p\s*,\s*"([^"]+)"\s*,\s*\d+\s*\)', fp[span[0]:span[1]]) if span else []
    files = [n for n in names if "." in n]
    folders = [n for n in names if "." not in n]
    if not files or not folders:
        out.append("files_paths.h: filePathIsKeyFile's names not found (%s)" % names)
    for kf in re.findall(r'^#define\s+AI_\w+_FILE\w*\s+"/(?:API Keys/)?([^"/]+\.txt)"', texts["ai_net.h"], re.M):
        if kf.lower() not in [n.lower() for n in files]:
            out.append("files_paths.h: %s (a key file ai_net.h reads) is no key file to filePathIsKeyFile - the Files "
                       "viewer would draw it and the clone script copy it" % kf)
    sh = texts[CLONE]
    em = re.search(r"^EXCL=\((.*?)\)\s*$", sh, re.M | re.S)
    excl = re.findall(r"--exclude\s+'([^']*)'", em.group(1)) if em else []
    cm = re.search(r"^count\(\)\s*\{(.*?)^\}", sh, re.M | re.S)
    cnt = cm.group(1) if cm else ""
    um = re.search(r"^usage\(\)\s*\{(.*?)^\}", sh, re.M | re.S)

    def globbed(name, suffix):
        for g in excl:
            if not g.endswith(suffix):
                continue
            rx = re.sub(r"\\\[([^\]]*)\\\]", r"[\1]", re.escape(g[:len(g) - len(suffix)] if suffix else g))
            if re.fullmatch(rx, name.lower()) and re.fullmatch(rx, name.upper()) and re.fullmatch(rx, name):
                return True
        return False
    for n in files:
        if not globbed(n, ""):
            out.append("%s: rsync copies a %s (no case-blind --exclude) - a cloned card would carry the key" % (CLONE, n))
        if not re.search(r"!\s+-iname\s+'%s'" % re.escape(n), cnt):
            out.append("%s: the count does not leave %s out (a key on one side reads as a short copy)" % (CLONE, n))
        if not um or n not in um.group(1):
            out.append("%s: the usage line does not say %s is left out" % (CLONE, n))
    for n in folders:
        if not globbed(n, "/"):
            out.append("%s: rsync copies the %s folder" % (CLONE, n))
        if not re.search(r"!\s+-ipath\s+'\*/%s/\*'" % re.escape(n), cnt):
            out.append("%s: the count does not leave the %s folder out" % (CLONE, n))


def check_topic(code, out):
    """Contract 21 (the X4's review, 2026-10-19): a topic never spans Gemini and Chutes."""
    ai, app = code["ai_net.cpp"], code["app_ai.cpp"]
    span = body(ai, "aiAsk", out, "ai_net.cpp")
    if span:
        w = ai[span[0]:span[1]]
        td = re.search(r"\bs_req\s*->\s*topic\s*=\s*chutesTopicDue\s*\(\s*chat\s*,\s*s_req\s*->\s*askedUtc\s*,\s*choice\s*,"
                       r"\s*s_m\s*->\s*topicLast\s*\)\s*;", w)
        if not td or len(re.findall(r"\bs_req\s*->\s*topic\s*=", w)) != 1:
            out.append("aiAsk: the question's topic is not chutesTopicDue(chat, s_req->askedUtc, choice, "
                       "s_m->topicLast), set once - a topic would span Gemini and Chutes")
        kept = re.search(r"\bs_m\s*->\s*topicLast\s*=\s*chutesChoiceProvider\s*\(\s*choice\s*\)\s*;", w)
        sub = re.search(r"\bnetWorkerSubmit\s*\(", w)
        if not kept or not td or not sub or kept.start() < td.start() or kept.start() > sub.start() or \
                re.search(r"\breturn\s+false\b", w[kept.end():]) or len(re.findall(r"\btopicLast\s*=", w)) != 1:
            out.append("aiAsk: this boot's last provider is not kept once (s_m->topicLast = chutesChoiceProvider(choice)) "
                       "after the topic is decided and the last refusal, before the question goes")
        cx = re.search(r"\bif\s*\(\s*!\s*s_req\s*->\s*topic\s*\)\s*\{[^}]*\bgeminiChatContext\s*\(\s*chat\s*,", w)
        if not cx or len(re.findall(r"\bgeminiChatContext\s*\(", w)) != 1:
            out.append("aiAsk: the topic's context is copied when the question starts a topic (if (!s_req->topic) {...})")
    span = body(ai, "memOk", out, "ai_net.cpp")
    if span and not re.search(r"\bs_m\s*->\s*topicLast\s*=\s*CHUTES_BY_UNKNOWN\s*;", ai[span[0]:span[1]]):
        out.append("memOk: this boot's last provider does not start UNKNOWN (calloc's 0 is CHUTES_BY_GEMINI)")
    at = body(ai, "aiTopicDue", out, "ai_net.cpp")
    if at:
        w = ai[at[0]:at[1]]
        if not re.search(r"\bconst\s+bool\s+due\s*=\s*chutesTopicDue\s*\(\s*c\s*,\s*now\s*,\s*aiModelChoice\s*\(\s*\)\s*,"
                         r"\s*s_m\s*->\s*topicLast\s*\)\s*;", w) or not re.search(r"\breturn\s+due\s*;", w):
            out.append("aiTopicDue: not aiAsk's rule with aiAsk's inputs (chutesTopicDue(c, now, aiModelChoice(), "
                       "s_m->topicLast))")
        if not re.search(r"\*\s*otherProvider\s*=\s*due\s*&&\s*!\s*geminiChatTopicDue\s*\(\s*c\s*,\s*now\s*\)\s*;", w):
            out.append("aiTopicDue: *otherProvider is not 'only the provider's rule' (due && !geminiChatTopicDue(c, now))")
    for name in ("ai_net.cpp", "app_ai.cpp", "serial_cmd.cpp"):
        for m in re.finditer(r"\bgeminiChatTopicDue\s*\(", code[name]):
            if name == "ai_net.cpp" and at and at[0] <= m.start() <= at[1] and \
                    statement_at(ai, m.start()) == "*otherProvider = due && !geminiChatTopicDue(c, now)":
                continue
            out.append("%s:%d: geminiChatTopicDue decides a topic - provider-blind (chutesTopicDue / aiTopicDue)"
                       % (name, line_of(code[name], m.start())))
    span = body(ai, "aiReport", out, "ai_net.cpp")
    if span and not re.search(r"\bconst\s+bool\s+topic\s*=\s*aiTopicDue\s*\(\s*&\s*other\s*\)\s*;", ai[span[0]:span[1]]):
        out.append("aiReport: `ai`'s next-question line is not aiTopicDue's (aiAsk's rule)")
    sc = code["serial_cmd.cpp"]
    m = re.search(r"if\s*\(\s*!\s*strcasecmp\s*\(\s*p\s*,\s*[^)]*\)\s*\|\|\s*!\s*strncasecmp\s*\(\s*p\s*,\s*[^)]*,\s*6\s*\)\s*\)\s*\{", sc)
    mb = sc[m.end():match_close(sc, m.end() - 1)] if m else ""
    if not re.search(r"\bif\s*\(\s*aiTopicDue\s*\(\s*&\s*otherProvider\s*\)\s*&&\s*otherProvider\s*\)\s*\{\s*say\s*\(", mb):
        out.append("serial_cmd.cpp: `ai model` does not say the next question starts a new topic after a change of provider")
    span = body(app, "AiApp::buildMenu", out, "app_ai.cpp")
    if span and not re.search(r"\bif\s*\(\s*aiTopicDue\s*\(\s*&\s*otherProvider\s*\)\s*&&\s*otherProvider\s*\)\s*\{\s*"
                              r"menu\s*->\s*addNoteWrapped\s*\(\s*CHUTES_TOPIC_SWITCH\s*\)\s*;", app[span[0]:span[1]]):
        out.append("app_ai.cpp: Menu > AI does not say the next question starts a new topic when only the provider's "
                   "rule starts one (aiTopicDue(&otherProvider) && otherProvider -> CHUTES_TOPIC_SWITCH)")


def _tok(s):
    """A C expression as a regex: its tokens, any spacing between them."""
    return r"\s*".join(re.escape(x) for x in re.findall(r"->|&&|\|\||[A-Za-z_]\w*|\d+|\S", s))


def _assigned(w, var, field, want):
    return re.search(r"\b%s\s*\.\s*%s\s*=\s*%s\s*;" % (var, _tok(field), _tok(want)), w) is not None


WEBTEXT = re.compile(r"\bs_work\s*->\s*(?:web|webSrc|webBody)\b")
SRCW = re.compile(r"\bs_work\s*->\s*src\b")


def check_web(texts, code, out):
    """Contracts 22-27 (D-176): the web search's wire, roots, decision and count, the block never stored, the
    search seen, and a bench search that leaves nothing behind."""
    ai, raw_ai = code["ai_net.cpp"], texts["ai_net.cpp"]
    ai_c = blank_comments(raw_ai)
    raw_wh = texts["websearch.h"]

    # 22. the Bearer goes only to api.tavily.com
    if define(raw_wh, "WEB_HOST") != '"api.tavily.com"':
        out.append("websearch.h: WEB_HOST is not \"api.tavily.com\": %s" % define(raw_wh, "WEB_HOST"))
    if define(raw_wh, "WEB_URL") != '"https://" WEB_HOST "/search"':
        out.append("websearch.h: WEB_URL is not https://WEB_HOST/search: %s" % define(raw_wh, "WEB_URL"))
    m = re.match(r"(\d+)u?\b", define(raw_wh, "WEB_ANSWER_MS") or "")
    if not m or int(m.group(1)) != 6000:
        out.append("websearch.h: WEB_ANSWER_MS is not 6000 (6 s for Tavily's answer: Nick, 2026-10-19, long budgets are fine): %s"
                   % define(raw_wh, "WEB_ANSWER_MS"))
    m = re.match(r"(\d+)u?\b", define(raw_ai, "AI_WEB_CONNECT_MS") or "")
    if not m or not 0 < int(m.group(1)) <= 10000:
        out.append("ai_net.cpp: AI_WEB_CONNECT_MS is %s - the handshake's cap, at most 10 s" % define(raw_ai, "AI_WEB_CONNECT_MS"))
    if re.sub(r"\s+", "", define(raw_ai, "AI_WEB_TOTAL_MS") or "") != "(AI_WEB_CONNECT_MS+WEB_ANSWER_MS)":
        out.append("ai_net.cpp: AI_WEB_TOTAL_MS is not (AI_WEB_CONNECT_MS + WEB_ANSWER_MS) - the whole search's budget")
    ws = body(ai, "aiWebSearch", out, "ai_net.cpp")
    if ws:
        s, e = ws
        w, rw = ai[s:e], ai_c[s:e]
        for field, want in (("host", "WEB_HOST"), ("url", "WEB_URL"), ("rootsPem", "AI_TAVILY_ROOTS_PEM"),
                            ("hdrValue[1]", "s_work->webAuth"), ("body", "(const uint8_t*)s_work->webBody"),
                            ("connectMs", "AI_WEB_CONNECT_MS"), ("timeoutMs", "WEB_ANSWER_MS"), ("waitSliceMs", "AI_SLICE_MS"),
                            ("totalMs", "AI_WEB_TOTAL_MS"), ("connectFirst", "true"), ("heapCheck", "true"),
                            ("stopFn", "tileFetchCallRecent"), ("sink", "netWorkerSink()"), ("cap", "0"),
                            ("tap", "webTap"), ("tapCtx", "&s_work->wp")):
            if not _assigned(w, "w", field, want):
                out.append("aiWebSearch: w.%s is not %s" % (field, want))
        if not re.search(r'\bw\s*\.\s*hdrName\s*\[\s*1\s*\]\s*=\s*"Authorization"\s*;', rw) or \
                not re.search(r'\bw\s*\.\s*method\s*=\s*"POST"\s*;', rw):
            out.append("aiWebSearch: not a POST with the key as the Authorization header")
        if not _assigned(w, "w", "cancel", "&s_cancel"):
            out.append("aiWebSearch: Cancel not honoured (w.cancel = &s_cancel)")
        auth = re.search(r"\bwebAuthHeader\s*\(\s*q\s*->\s*web\s*\.\s*key\s*,\s*s_work\s*->\s*webAuth\s*,", w)
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        wipe = re.search(r"\bmemset\s*\(\s*s_work\s*->\s*webAuth\s*,\s*0\s*,", w[net[0].start():]) if net else None
        if not auth or len(net) != 1 or auth.start() > net[0].start():
            out.append("aiWebSearch: the header is not built from the TAVILY key (webAuthHeader) before its ONE netHttps")
        elif not wipe or re.search(r"\breturn\b", w[net[0].start():net[0].start() + wipe.start()]):
            out.append("aiWebSearch: the header is not wiped right after netHttps returns")
        if re.search(r"\bfor\s*\(|\bwhile\s*\(|\bgoto\b", w):
            out.append("aiWebSearch: a loop - the search is never retried")
        if re.search(r"\b(?:cfg|chutes)\s*\.\s*key\b|\bs_work\s*->\s*auth\b|\bctxA\b", w) or "x-goog-api-key" in rw:
            out.append("aiWebSearch: another provider's key, or the chat, in the search")
        bb = args_of(ai, s, e, "webBuildRequest")
        if len(bb) != 1 or [x.strip() for x in bb[0][1].split(",")][0] != "q->question":
            out.append("aiWebSearch: the body is not built once from the plain question (webBuildRequest(q->question, ...))")
    for fn in ("postOnce", "postOnceChutes", "aiWikiLookup", "aiRunChutes"):
        sp = function_body(ai, fn)
        if sp and re.search(r"\bweb\s*\.\s*key\b|\bwebAuth\b|\bs_wcfg\b|\bwebAuthHeader\b", ai[sp[0]:sp[1]]):
            out.append("%s: the Tavily key in another request" % fn)
    for m in re.finditer(r"\bwebAuthHeader\s*\(", ai):
        if not ws or not (ws[0] <= m.start() <= ws[1]):
            out.append("ai_net.cpp:%d: webAuthHeader outside aiWebSearch - the Tavily Bearer goes nowhere else" % line_of(ai, m.start()))
    tap = body(ai, "webTap", out, "ai_net.cpp")
    if tap and re.sub(r"\s+", "", ai[tap[0]:tap[1]]).strip("{}") != "webParseFeed((WebParse*)ctx,p,n);":
        out.append("webTap: does more than feed the parser (the body is never kept)")

    # 23. the roots
    roots = define(raw_ai, "AI_TAVILY_ROOTS_PEM")
    if roots is None or roots.split() != ["AMAZON_ROOT_CA1_PEM", "STARFIELD_SERVICES_ROOT_G2_PEM"]:
        out.append("ai_net.cpp: AI_TAVILY_ROOTS_PEM is not Amazon Root CA 1 + Starfield Services Root G2: %s" % roots)
    am = texts["amazon_roots.h"]
    for fp in ("8E:CD:E6:88:4F:3D:87:B1:12:5B:A3:1A:C3:FC:B1:3D:70:16:DE:7F:57:CC:90:4F:E1:CB:97:C6:AE:98:19:6E",
               "56:8D:69:05:A2:C8:87:08:A4:B3:02:51:90:ED:CF:ED:B1:97:4A:60:6A:13:C6:E5:29:0F:CB:2A:E6:3E:DA:B5"):
        if fp not in am:
            out.append("amazon_roots.h: the fingerprint %s... is not recorded" % fp[:11])

    # 24. the decision, the key and the count
    sp = body(ai, "aiAsk", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        dec = args_of(ai, sp[0], sp[1], "webDecide")
        if len(dec) != 1 or [x.strip() for x in dec[0][1].split(",")] != \
                ["aiWebMode()", "aiWebUsable()", "forceWeb", "s_req->question", "webDayAutoUsed(&s_m->day", "webToday())",
                 "&s_req->webDec"]:
            out.append("aiAsk: webDecide is not given the setting, a GOOD tavily.txt, the force, the plain question and "
                       "today's automatic count: %s" % (dec[0][1] if dec else "no call"))
        dl = re.search(r"\bdayLoad\s*\(\s*\)\s*;", w)
        if not dl or not dec or sp[0] + dl.start() > dec[0][0]:
            out.append("aiAsk: today's count is not read from NVS (dayLoad) before the decision - a reboot would reset the cap")
        if not re.search(r"\bif\s*\(\s*s_req\s*->\s*webDec\s*\.\s*search\s*\)\s*\{\s*memcpy\s*\(\s*&\s*s_req\s*->\s*web\s*,\s*s_wcfg\s*,"
                         r"[^}]*\}\s*else\s*\{\s*memset\s*\(\s*&\s*s_req\s*->\s*web\s*,\s*0\s*,", w) or \
                len(re.findall(r"\bmemcpy\s*\(\s*&\s*s_req\s*->\s*web\b", w)) != 1:
            out.append("aiAsk: the Tavily key goes with a question that does not search")
    sp = body(ai, "aiWebTest", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        off = re.search(r"\bif\s*\(\s*aiWebMode\s*\(\s*\)\s*==\s*WEB_OFF\s*\)\s*\{[^}]*\breturn\s+false\s*;", w)
        key = re.search(r"\bif\s*\(\s*!\s*aiWebUsable\s*\(\s*\)\s*\)\s*\{[^}]*\breturn\s+false\s*;", w)
        sub = re.search(r"\bnetWorkerSubmit\s*\(", w)
        if not off or not key or not sub or off.start() > sub.start() or key.start() > sub.start():
            out.append("aiWebTest: a bench search under Off, or without a usable tavily.txt")
        if not re.search(r"\bmemset\s*\(\s*&\s*s_req\s*->\s*cfg\s*,\s*0\s*,", w) or \
                not re.search(r"\bmemset\s*\(\s*&\s*s_req\s*->\s*chutes\s*,\s*0\s*,", w) or \
                not re.search(r"\bs_req\s*->\s*webOnly\s*=\s*true\s*;", w):
            out.append("aiWebTest: the search alone must carry no other key (cfg and chutes zeroed, webOnly)")
    sp = body(ai, "aiWebUsable", out, "ai_net.cpp")
    if sp and not re.search(r"\breturn\s+memOk\s*\(\s*\)\s*&&\s*s_m\s*->\s*wkey\s*\.\s*usable\s*&&\s*s_wcfg\s*&&\s*s_wHaveGood\s*;",
                            ai[sp[0]:sp[1]]):
        out.append("aiWebUsable: not a GOOD tavily.txt read (wkey.usable && s_wcfg && s_wHaveGood)")
    sp = body(ai, "aiRun", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        calls = list(re.finditer(r"\baiWebSearch\s*\(", w))
        only = re.search(r"\bif\s*\(\s*q\s*->\s*webOnly\s*\)\s*\{\s*aiWebSearch\s*\(\s*q\s*,\s*r\s*\)\s*;[^}]*\breturn\s*;\s*\}", w)
        lad = re.search(r"\b(?:chutesLadderStart|geminiLadderStart)\s*\(", w)
        wl = re.search(r"\baiWikiLookup\s*\(", w)
        rc = re.search(r"\baiRunChutes\s*\(", w)
        po = re.search(r"\bpostOnce\s*\(", w)
        main_ = [c for c in calls if not (only and only.start() <= c.start() <= only.end())]
        if len(calls) != 2 or not only or not lad or only.start() > lad.start():
            out.append("aiRun: the bench-only search does not return before any ladder (if (q->webOnly) {...return;})")
        if len(main_) != 1 or not wl or not rc or not po or not (wl.start() < main_[0].start() < rc.start() < po.start()):
            out.append("aiRun: the search is not made ONCE, after the lookup and before either provider")
    sp = body(ai, "aiLoopTick", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        cnt = re.search(r"\bif\s*\(\s*s_res\s*->\s*webAsked\s*&&\s*memOk\s*\(\s*\)\s*\)\s*\{\s*dayLoad\s*\(\s*\)\s*;\s*"
                        r"if\s*\(\s*s_res\s*->\s*webSent\s*\)\s*\{\s*"
                        r"webDayCount\s*\(\s*&\s*s_m\s*->\s*day\s*,\s*webToday\s*\(\s*\)\s*,\s*s_req\s*->\s*webDec\s*\.\s*automatic\s*\)\s*;"
                        r"\s*daySave\s*\(\s*\)\s*;\s*\}", w)
        wo = re.search(r"\bif\s*\(\s*s_req\s*->\s*webOnly\s*\)\s*\{", w)
        if not cnt or not wo or cnt.start() > wo.start() or len(re.findall(r"\bwebDayCount\s*\(", ai)) != 1:
            out.append("aiLoopTick: a SENT search is not counted (webDayCount + daySave under s_res->webSent, Auto's apart) "
                       "before anything else - or a search is counted somewhere else")
        wipes = list(re.finditer(r"\bmemset\s*\(\s*s_req\s*->\s*web\s*\.\s*key\s*,\s*0\s*,", w))
        wend = match_close(w, w.find("{", wo.end() - 1)) if wo else -1
        if not wo or not any(wo.start() < x.start() < wend for x in wipes) or not any(x.start() > wend for x in wipes):
            out.append("aiLoopTick: the Tavily key copy is not wiped on both paths (the bench search's and the fold's)")
    if ws:
        w = ai[ws[0]:ws[1]]
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        sent = re.search(r"\br\s*->\s*webSent\s*=\s*res\s*\.\s*largestAfterSend\s*!=\s*0\s*\|\|\s*code\s*>\s*0\s*;", w)
        if not sent or not net or sent.start() < net[0].start() or len(re.findall(r"\bwebSent\s*=", ai)) != 1:
            out.append("aiWebSearch: the search is not marked SENT only once its request was written (r->webSent = "
                       "res.largestAfterSend != 0 || code > 0, after netHttps, and nowhere else) - a search that never "
                       "reached Tavily would use up the day's automatic ones")
    sp = body(ai, "webToday", out, "ai_net.cpp")
    if sp and not re.search(r"\breturn\s+webLocalDay\s*\(\s*utcNow\s*\(\s*\)\s*,\s*tzNow\s*\(\s*\)\s*\)\s*;", ai[sp[0]:sp[1]]):
        out.append("webToday: not a trusted clock's local day (webLocalDay(utcNow(), tzNow()))")
    for name, want in (("AI_PREFS_WEB", '"ai_web"'), ("AI_PREFS_WEBD", '"ai_webd"'), ("AI_PREFS_WEBN", '"ai_webn"'),
                       ("AI_PREFS_WEBO", '"ai_webo"')):
        v = ((define(raw_ai, name) or "").split() or [""])[0]
        if v != want or not re.fullmatch(r'"[a-z_]{1,15}"', v):
            out.append("ai_net.cpp: %s is not %s (an NVS key: 15 characters at most)" % (name, want))
    sp = body(ai, "webModeLoad", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        ik = re.search(r"\bp\s*\.\s*isKey\s*\(\s*AI_PREFS_WEB\s*\)", w)
        gs = re.search(r"\bp\s*\.\s*getString\s*\(\s*AI_PREFS_WEB\s*,", w)
        if not ik or not gs or gs.start() < ik.start():
            out.append("webModeLoad: getString without isKey first (an absent ai_web logs an error line)")
    sp = body(ai, "aiSetWebMode", out, "ai_net.cpp")
    if sp:
        w = ai[sp[0]:sp[1]]
        if not re.search(r"\bmode\s*<\s*0\s*\|\|\s*mode\s*>=\s*WEB_MODES\b", w) or \
                not re.search(r"\bstrlcpy\s*\(\s*s_m\s*->\s*webStored\s*,\s*webModeId\s*\(\s*mode\s*\)", w):
            out.append("aiSetWebMode: stores something that is not an allowlisted id")
    for fn, wipes in (("aiJob", (r"s_req\s*->\s*web\s*\.\s*key", r"s_work\s*->\s*webAuth")),
                      ("aiLoopTick", (r"s_req\s*->\s*web\s*\.\s*key",))):
        sp = body(ai, fn, out, "ai_net.cpp")
        if sp:
            for wrx in wipes:
                if not re.search(r"\bmemset\s*\(\s*%s\s*,\s*0\s*," % wrx, ai[sp[0]:sp[1]]):
                    out.append("%s: the Tavily key copy (%s) is not wiped" % (fn, wrx.replace("\\s*", "")))
    if not (define(texts["ai_net.h"], "AI_TAVILY_FILE") or "").startswith('"/API Keys/tavily.txt"'):
        out.append("ai_net.h: AI_TAVILY_FILE is not \"/API Keys/tavily.txt\"")
    sp = body(ai, "webKeyReload", out, "ai_net.cpp")
    if sp:
        w = ai_c[sp[0]:sp[1]]
        if re.search(r'"/[^"]*tavily\.txt"|AI_KEY_FILE|AI_CHUTES_FILE', w) or len(re.findall(r"\bAI_TAVILY_FILE\b", w)) < 2:
            out.append("webKeyReload: tavily.txt is read from somewhere other than AI_TAVILY_FILE")
        if not re.search(r"\bwebParseConfig\s*\(\s*buf\s*,", ai[sp[0]:sp[1]]):
            out.append("webKeyReload: tavily.txt is not parsed by webParseConfig (tvly- only)")
    span = function_body(ai, "aiKeyReload")
    if span and not re.search(r"\bwebKeyReload\s*\(\s*\)\s*;", ai[span[0]:span[1]]):
        out.append("aiKeyReload: tavily.txt is not read with the other two (re-read before every question)")
    sc = code["serial_cmd.cpp"]
    m = re.search(r"if\s*\(\s*!\s*strcasecmp\s*\(\s*p\s*,\s*[^)]*\)\s*\|\|\s*!\s*strncasecmp\s*\(\s*p\s*,\s*[^)]*,\s*4\s*\)\s*\)\s*\{\s*"
                  r"const\s+char\s*\*\s*a\s*=\s*p\s*\+\s*3\s*;", sc)
    if not m:
        out.append("serial_cmd.cpp: the `ai web` branch not found")
    else:
        bs = m.end()
        be = match_close(sc, sc.rfind("{", 0, m.end()))
        for pos, a in args_of(sc, bs, be, "say"):
            if re.search(r"\ba\b", a) or re.search(r"\bq\b", re.sub(r"\bstrlen\s*\(\s*q\s*\)", "", a)):
                out.append("serial_cmd.cpp: `ai web` echoes what was typed (a key pasted there would reach the log)")
                break
    app = code["app_ai.cpp"]
    sp = function_body(app, "AiApp::buildMenu")
    if not sp or not re.search(r"\bif\s*\(\s*webUsable\s*&&\s*webMode\s*!=\s*WEB_OFF\s*\)\s*\{\s*menu\s*->\s*addOption\s*\(\s*WEB_ASK_ROW\s*,"
                               r"\s*ROW_ASKWEB\s*\)", app[sp[0]:sp[1]]):
        out.append("app_ai.cpp: \"Ask with web search\" is offered under Off or without a key")
    pe = function_body(app, "AiApp::processEvent")
    if not pe or not re.search(r"\bk\s*==\s*ROW_WEB\s*\)\s*\{[^}]*\bif\s*\(\s*!\s*aiWebUsable\s*\(\s*\)\s*\)\s*\{\s*enterState\s*\(\s*AI_KEYINFO\s*\)\s*;"
                               r"\s*\}\s*else\s*\{\s*aiSetWebMode\s*\(\s*webModeNext\s*\(\s*aiWebMode\s*\(\s*\)\s*\)\s*\)\s*;", app[pe[0]:pe[1]]):
        out.append("app_ai.cpp: the Web search row cycles without a usable tavily.txt (it must open Key info)")
    mb = None
    if pe:
        seg = app[pe[0]:pe[1]]
        m1 = re.search(r"\bcase\s+AI_MENU\s*:", seg)
        m2 = re.search(r"\bcase\s+AI_\w+\s*:", seg[m1.end():]) if m1 else None
        mb = (seg[m1.end():m1.end() + m2.start()] if m2 else seg[m1.end():]) if m1 else None
    if not mb or not re.search(r"\bif\s*\(\s*k\s*==\s*ROW_ASK\s*\|\|\s*k\s*==\s*ROW_ASKWEB\s*\)\s*\{[^{}]*?"
                               r"\baskWeb\s*=\s*k\s*==\s*ROW_ASKWEB\s*&&\s*next\s*==\s*AI_COMPOSE\s*;\s*enterState\s*\(\s*next\s*\)\s*;", mb):
        out.append("app_ai.cpp: OK on \"Ask with web search\" does not open the compose with the flag set (handled with "
                   "ROW_ASK, askWeb = k == ROW_ASKWEB && next == AI_COMPOSE before enterState(next))")
    sp = function_body(app, "AiApp::enterState")
    if not sp or not re.search(r"\bif\s*\(\s*s\s*!=\s*AI_COMPOSE\s*\)\s*\{\s*askWeb\s*=\s*false\s*;", app[sp[0]:sp[1]]):
        out.append("app_ai.cpp: \"Ask with web search\" outlives its compose (askWeb not reset)")
    sp = function_body(app, "AiApp::ask")
    if not sp or not re.search(r"\baiAsk\s*\(\s*q\s*,\s*noteBuf\s*,\s*sizeof\s*\(\s*noteBuf\s*\)\s*,\s*askWeb\s*\)", app[sp[0]:sp[1]]):
        out.append("app_ai.cpp: the compose's web flag never reaches aiAsk")

    # 25. the block is never stored
    wsr = function_body(ai, "aiWebReport")
    wtest = function_body(ai, "aiWebTest")
    for m in WEBTEXT.finditer(ai):
        if ws and ws[0] <= m.start() <= ws[1]:
            continue
        if wtest and wtest[0] <= m.start() <= wtest[1] and \
                re.fullmatch(r"memset\(s_work->(web|webSrc), 0, sizeof\(s_work->\1\)\)", statement_at(ai, m.start())):
            continue                                   # contract 27: the test WIPES the last block first
        if wsr and wsr[0] <= m.start() <= wsr[1] and m.group(0).replace(" ", "") in ("s_work->web", "s_work->webSrc"):
            g = re.search(r"\bif\s*\(\s*!\s*s_req\s*->\s*webOnly\s*\)\s*\{\s*return\s*;", ai[wsr[0]:wsr[1]])
            if g and wsr[0] + g.end() < m.start():
                continue
        out.append("ai_net.cpp:%d: %s outside aiWebSearch (and `ai web test`'s guarded print) - the block is never stored"
                   % (line_of(ai, m.start()), m.group(0)))
        break
    stored = re.compile(r"\bs_work\s*->\s*(?:bg|web|webSrc|webBody)\b")
    for fn in ("log_e", "log_w", "log_i", "log_d", "log_v", "printf", "geminiChatFold", "geminiChatAdd", "chatSave",
               "writeFile", "strlcpy", "snprintf"):
        for pos, a in args_of(ai, 0, len(ai), fn):
            if stored.search(a):
                out.append("ai_net.cpp:%d: %s(%s) - the background or the web block in a log, a text or the chat"
                           % (line_of(ai, pos), fn, a[:50]))
    wk = function_body(ai, "aiWikiLookup")
    uses_allowed(ai, SRCW, (("wikiCompose", 5), ("webAppendLine", 0), ("sizeof", None), ("geminiAnswerText", 2),
                            ("chutesAnswerShown", 3), ("emitWrapped", 2)),
                 "ai_net.cpp", "the sources line written outside wikiCompose, webAppendLine and the clears", out,
                 statements=(r"s_work->src\[0\] =.*",))
    if not wk or not re.match(r"\{\s*s_work\s*->\s*bg\s*\[\s*0\s*\]\s*=[^;]*;\s*s_work\s*->\s*src\s*\[\s*0\s*\]\s*=[^;]*;", ai[wk[0]:wk[1]]):
        out.append("aiWikiLookup: the background and the sources line are not cleared before anything (the last question's would go)")

    # 26. the search is seen: its stage, and why nothing went, under the answer
    if ws:
        s, e = ws
        w = ai[s:e]
        st = re.search(r"\bprogSet\s*\(\s*WEB_STAGE\s*,", w)
        net = list(re.finditer(r"\bnetHttps\s*\(", w))
        if not st or not net or st.start() > net[0].start():
            out.append("aiWebSearch: the stage is not \"Searching the web...\" (progSet(WEB_STAGE, ...)) before its request")
        lines = [re.sub(r"\s+", "", a) for _, a in args_of(ai, s, e, "webAppendLine")]
        want = ["s_work->src,sizeof(s_work->src),webNoteLine(r->webNote)",
                "s_work->src,sizeof(s_work->src),webNoteLine(r->webNote)",
                "s_work->src,sizeof(s_work->src),note==WEB_NOTE_NONE?s_work->webSrc:webNoteLine(note)"]
        if lines != want:
            out.append("aiWebSearch: not every way out says why under the answer (the cap's note, the no-key note, then "
                       "the result's line or its note): %s" % lines)
        if not re.search(r"\br\s*->\s*webNote\s*=\s*q\s*->\s*webNote\s*;\s*if\s*\(\s*!\s*q\s*->\s*webDec\s*\.\s*search\s*\)\s*\{\s*"
                         r"if\s*\(\s*r\s*->\s*webNote\s*!=\s*WEB_NOTE_NONE\s*\)\s*\{\s*webAppendLine\s*\(", w):
            out.append("aiWebSearch: a question the cap kept from searching gets no note under its answer")
        if not re.search(r"\br\s*->\s*webNote\s*=\s*WEB_NOTE_UNAVAILABLE\s*;\s*webAppendLine\s*\([^;]*\)\s*;\s*return\s*;", w):
            out.append("aiWebSearch: no body or no key that can go, and no \"Web: search unavailable\" under the answer")
        if not re.search(r"\br\s*->\s*webNote\s*=\s*note\s*;\s*webAppendLine\s*\([^;]*\)\s*;\s*$", w):
            out.append("aiWebSearch: the result's line or its note is not the LAST thing it does (r->webNote = note; webAppendLine)")
    sp = function_body(ai, "aiAsk")
    if sp:
        w = ai[sp[0]:sp[1]]
        cap = re.search(r"\bs_req\s*->\s*webNote\s*=\s*s_req\s*->\s*webDec\s*\.\s*why\s*==\s*WEB_WHY_CAP\s*\?\s*WEB_NOTE_CAP\s*:"
                        r"\s*WEB_NOTE_NONE\s*;", w)
        dec = re.search(r"\bwebDecide\s*\(", w)
        if not cap or not dec or cap.start() < dec.start():
            out.append("aiAsk: the cap's note never reaches the answer (s_req->webNote = ...why == WEB_WHY_CAP ? WEB_NOTE_CAP : "
                       "WEB_NOTE_NONE, after webDecide)")

    # 27. a bench search leaves nothing behind
    sp = function_body(ai, "aiLoopTick")
    if sp:
        w = ai[sp[0]:sp[1]]
        wo = re.search(r"\bif\s*\(\s*s_req\s*->\s*webOnly\s*\)\s*\{", w)
        wend = match_close(w, wo.end() - 1) if wo else -1
        br = w[wo.end():wend] if wo and wend > 0 else ""
        for flag in ("s_breakAfter", "s_clearAfter"):
            if not re.search(r"\b%s\s*=\s*false\s*;" % flag, br) or not re.search(r"\breturn\s*;", br):
                out.append("aiLoopTick: `ai web test`'s branch leaves %s latched - the NEXT question's answer is dropped "
                           "or its topic broken again" % flag)
    sp = function_body(ai, "aiWebTest")
    if sp:
        w = ai[sp[0]:sp[1]]
        sub = re.search(r"\bnetWorkerSubmit\s*\(", w)
        for buf in ("web", "webSrc"):
            m = re.search(r"\bmemset\s*\(\s*s_work\s*->\s*%s\s*,\s*0\s*,\s*sizeof\s*\(\s*s_work\s*->\s*%s\s*\)\s*\)\s*;" % (buf, buf), w)
            if not m or not sub or m.start() > sub.start():
                out.append("aiWebTest: s_work->%s not wiped before the test is submitted (`ai web` could print an earlier "
                           "question's)" % buf)
    if wsr:
        w = ai[wsr[0]:wsr[1]]
        g = re.search(r"\bif\s*\(\s*s_res\s*->\s*webAsked\s*&&\s*s_res\s*->\s*webNote\s*==\s*WEB_NOTE_NONE\s*&&\s*s_work\s*&&\s*"
                      r"s_work\s*->\s*web\s*\[\s*0\s*\]\s*\)\s*\{", w)
        ge = match_close(w, g.end() - 1) if g else -1
        prints = [m.start() for m in re.finditer(r"\bemitWrapped\s*\(\s*emit\s*,[^;]*\bs_work\s*->\s*(?:web|webSrc)\s*\)", w)]
        if not g or not prints or not all(g.end() <= x < ge for x in prints):
            out.append("aiWebReport: the block is printed for a test whose search never ran or put none in (it must be "
                       "s_res->webAsked && s_res->webNote == WEB_NOTE_NONE && s_work->web[0])")
    if ws:
        w = ai[ws[0]:ws[1]]
        ab = re.search(r"\bwebAppendBlock\s*\(", w)
        wipe = re.search(r"\bif\s*\(\s*!\s*q\s*->\s*webOnly\s*\)\s*\{\s*memset\s*\(\s*s_work\s*->\s*web\s*,\s*0\s*,\s*"
                         r"sizeof\s*\(\s*s_work\s*->\s*web\s*\)\s*\)\s*;\s*\}", w)
        if not ab or not wipe or wipe.start() < ab.start():
            out.append("aiWebSearch: a question's block copy outlives it (if (!q->webOnly) memset(s_work->web, ...) after "
                       "webAppendBlock)")


_BLANKED = {}


def blank_comments(t):
    """Comments blanked to spaces (newlines kept), string literals KEPT: the same offsets as
    strip_code's, so a function_body span on the one indexes the other. (Cached: the self-test
    asks it of the same text once per mutation of another file.)"""
    key = hash(t)
    if key not in _BLANKED:
        _BLANKED[key] = _blank_comments(t)
    return _BLANKED[key]


def _blank_comments(t):
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", t[i:j]))
            i = j
        elif t[i] in "\"'":
            q = t[i]
            j = i + 1
            while j < n and t[j] != q:
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def strip_comments(t):
    """Comments out, string literals KEPT (strip_code blanks both)."""
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            i = n if j < 0 else j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif t[i] == '"':
            j = i + 1
            while j < n and t[j] != '"':
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        elif t[i] == "'":
            j = i + 1
            while j < n and t[j] != "'":
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1])
            i = j + 1
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


_SHOWN_CALL = ("    chutesAnswerShown(rep, l.cur, s_work->text, s_work->src[0] ? s_work->src : NULL, r->answer, "
               "sizeof(r->answer));\n")


def _move_shown_after_ladder(t):
    """chutesAnswerShown's call moved to just after chutesLadderNext's (the ladder reads the parse's TEXT)."""
    ladder = "    const int step = chutesLadderNext(&l, code, net, rep, millis(), &delayMs, &stage);\n"
    if _SHOWN_CALL not in t or ladder not in t:
        return t
    return t.replace(_SHOWN_CALL, "", 1).replace(ladder, ladder + _SHOWN_CALL, 1)


def _move_search_before_lookup(t):
    """aiRun's search made before the Wikipedia lookup (the spec: Wikipedia first, then the web)."""
    call = "  aiWebSearch(q, r);\n  if (s_cancel) {"
    look = "  aiWikiLookup(q, r);\n"
    if call not in t or look not in t:
        return t
    return t.replace(call, "  if (s_cancel) {", 1).replace(look, "  aiWebSearch(q, r);\n" + look, 1)


def _drop_main_search(t):
    """aiRun's search after the lookup removed (the bench's stays)."""
    call = "  aiWebSearch(q, r);\n  if (s_cancel) {"
    return t.replace(call, "  if (s_cancel) {", 1)


MUTATIONS = [
    ("the resume probe stats a key file", "app_gbc_xfer.cpp",
     lambda t: t.replace("  if (xferKeyPath(path)) {\n    return 0;", "  if (false) {\n    return 0;", 1)),
    ("a key file resumed from the card", "app_gbc_xfer.cpp",
     lambda t: t.replace("    if (off != 0 && xferKeyPath(path)) {", "    if (false) {", 1)),
    ("a key file is whatever the uploader says", "app_gbc_xfer.cpp",
     lambda t: t.replace("  return filePathIsKeyFile(path.c_str()) != 0;", "  return false;", 1)),
    ("the legacy GET /chunk stats the card itself", "app_gbc_xfer.cpp",
     lambda t: t.replace("  const size_t held = xferHeld(name);\n  char out[16];",
                         "  File kf = SD.open((String(s_cfg->dir) + \"/\" + name).c_str());\n"
                         "  const size_t held = kf ? (size_t)kf.size() : 0;\n  char out[16];", 1)),
    ("a route that reads a file", "app_gbc_xfer.cpp",
     lambda t: t.replace("static void handleNotFound() {\n",
                         "static void handleNotFound() {\n  File peek = SD.open(s_server->uri().c_str());\n", 1)),
    ("a route that lists a folder", "app_gbc_xfer.cpp",
     lambda t: t.replace("static void handleNotFound() {\n",
                         "static void handleNotFound() {\n  File d = SD.open(s_cfg->dir, FILE_WRITE); d.openNextFile();\n", 1)),
    ("the 60 s timeout back", "ai_net.cpp",
     lambda t: t.replace("#define AI_TIMEOUT_MS       150000u", "#define AI_TIMEOUT_MS       60000u", 1)),
    ("no slices for the wait", "ai_net.cpp",
     lambda t: t.replace("  q.waitSliceMs = AI_SLICE_MS;", "", 1)),
    ("a call does not end Gemini's wait", "ai_net.cpp",
     lambda t: t.replace("  q.stopFn = tileFetchCallRecent;", "", 1)),
    ("timeoutMs a uint16 again", "https_worker.h",
     lambda t: t.replace("  uint32_t     timeoutMs;", "  uint16_t     timeoutMs;", 1)),
    ("the unclamped setTimeout", "https_worker.cpp",
     lambda t: re.sub(r"http\.setTimeout\([^;]*;", "http.setTimeout(q->timeoutMs);", t, count=1)),
    ("a plain WiFiClientSecure", "https_worker.cpp",
     lambda t: t.replace("NetClient* c = new (std::nothrow) NetClient();", "WiFiClientSecure* c = new (std::nothrow) WiFiClientSecure();", 1)),
    ("never armed", "https_worker.cpp",
     lambda t: t.replace("c->armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn);", ";", 1)),
    ("armed without the call stop", "https_worker.cpp",
     lambda t: t.replace("c->armFirstByte(wait, q->waitSliceMs, q->cancel, q->stopFn);", "c->armFirstByte(wait, q->waitSliceMs, q->cancel, NULL);", 1)),
    ("armed for every request", "https_worker.cpp",
     lambda t: t.replace("if (q->waitSliceMs || q->timeoutMs > NET_HTTP_TIMEOUT_MAX) {", "if (true) {", 1)),
    ("the deadline called LOST", "https_worker.cpp",
     lambda t: t.replace("        } else if (c->timedOut()) {\n          r->net = GEM_NET_TIMEOUT;", "        } else if (c->timedOut()) {\n          r->net = GEM_NET_LOST;", 1)),
    ("no stop check after DNS", "https_worker.cpp",
     lambda t: t.replace("  if (netStopped(q)) {\n    r->net = GEM_NET_LOST;\n    r->ms = millis() - t0;\n    return 0;\n  }", "", 1)),
    ("the connect not shortened to the budget", "https_worker.cpp",
     lambda t: t.replace("connected = c->connect(q->host, 443, (int32_t)connectMs) == 1;", "connected = c->connect(q->host, 443, (int32_t)q->connectMs) == 1;", 1)),
    ("the body past the budget", "https_worker.cpp",
     lambda t: t.replace("if (!readBody(q, r, http, chunked, http.getSize(), t0)) {", "if (!readBody(q, r, http, chunked, http.getSize(), millis())) {", 1)),
    ("the body ignores a call", "https_worker.cpp",
     lambda t: t.replace("  return (q->cancel && *q->cancel) || (q->stopFn && q->stopFn());", "  return (q->cancel && *q->cancel);", 1)),
    ("the wait before the request is written", "https_worker.cpp",
     lambda t: t.replace("    if (!m_armed || !m_sent) {\n      return n;", "    if (!m_armed) {\n      return n;", 1)),
    ("Cancel ignored in the wait", "https_worker.cpp",
     lambda t: t.replace("      if ((m_cancel && *m_cancel) || (m_stopFn && m_stopFn())) {", "      if (m_stopFn && m_stopFn()) {", 1)),
    ("a call ignored in the wait", "https_worker.cpp",
     lambda t: t.replace("      if ((m_cancel && *m_cancel) || (m_stopFn && m_stopFn())) {", "      if (m_cancel && *m_cancel) {", 1)),
    ("no stop at the deadline", "https_worker.cpp",
     lambda t: t.replace("        m_timedOut = true;\n        stop();", "        m_timedOut = true;", 1)),
    ("a spinning wait", "https_worker.cpp",
     lambda t: t.replace("      vTaskDelay(pdMS_TO_TICKS(m_sliceMs));", "", 1)),
    ("the virtual connected() in the wait", "https_worker.cpp",
     lambda t: t.replace("      if (k != 0 || !_connected) {", "      if (k != 0 || !connected()) {", 1)),
    ("a short write arms the wait", "https_worker.cpp",
     lambda t: t.replace("if (size > 0 && n == size) {", "if (size > 0) {", 1)),
    ("the date from an untrusted clock", "ai_net.cpp",
     lambda t: t.replace("geminiSystemText(utcNow(),", "geminiSystemText((int64_t)ntpClock.getExactUtcTime(),", 1)),
    ("the fixed instruction sent", "ai_net.cpp",
     lambda t: t.replace("s_work->bg, s_work->sys, l.budget, s_body", "s_work->bg, GEM_SYSTEM_INSTRUCTION, l.budget, s_body", 1)),
    ("no background sent", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, l.budget, s_body", "q->question, NULL, s_work->sys, l.budget, s_body", 1)),
    ("the background glued into the question (and so the chat)", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, l.budget,\n", "s_work->bg, q->question, s_work->sys, l.budget,\n", 1)),
    ("the answer not scrubbed", "ai_net.cpp",
     lambda t: t.replace("        geminiScrub(r->answer, sizeof(r->answer), q->cfg.key);\n", "", 1)),
    ("the web's note hidden when nothing went (Gemini's sources tied to background again)", "ai_net.cpp",
     lambda t: t.replace("geminiAnswerText(rep, s_work->text, s_work->src[0] ? s_work->src : NULL,",
                         "geminiAnswerText(rep, s_work->text, s_work->bg[0] ? s_work->src : NULL,", 1)),
    ("utcNow trusting a mesh clock", "ai_net.cpp",
     lambda t: t.replace("return (int64_t)ntpClock.getTrustedUtcTime();", "return (int64_t)ntpClock.getExactUtcTime();", 1)),
    ("a zone name for another offset", "ai_net.cpp",
     lambda t: t.replace("if (e.ok && e.name && e.minutes * 60 == s_req->tzS) {", "if (e.name) {", 1)),
    ("a call after the request not told apart", "ai_net.cpp",
     lambda t: t.replace("if (code <= 0 && !s_cancel && tileFetchCallRecent()) {", "if (false) {", 1)),
    ("the lookup per retry", "ai_net.cpp",
     lambda t: t.replace("  aiWikiLookup(q, r);\n", "", 1).replace("    const uint32_t ta = millis();\n", "    aiWikiLookup(q, r);\n    const uint32_t ta = millis();\n", 1)),
    ("the lookup never made", "ai_net.cpp",
     lambda t: t.replace("  aiWikiLookup(q, r);\n", "", 1)),
    ("lookup=off ignored", "ai_net.cpp",
     lambda t: t.replace("  if (!q->cfg.lookup) {\n", "  if (false) {\n", 1)),
    ("the whole question to Wikipedia", "ai_net.cpp",
     lambda t: t.replace("wikiUrl(s_work->kw, s_work->wikiUrl,", "wikiUrl(q->question, s_work->wikiUrl,", 1)),
    ("a retried lookup", "ai_net.cpp",
     lambda t: t.replace("  int code = netHttps(&w, &res);", "  int code = 0;\n  for (int i = 0; i < 2 && code != 200; i++) code = netHttps(&w, &res);", 1)),
    ("the key sent to Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.userAgent = WIKI_USER_AGENT;", "  w.userAgent = WIKI_USER_AGENT;\n  w.hdrName[0] = \"x-goog-api-key\";\n  w.hdrValue[0] = q->cfg.key;", 1)),
    ("a generic User-Agent", "ai_net.cpp",
     lambda t: t.replace("  w.userAgent = WIKI_USER_AGENT;", "  w.userAgent = AI_USER_AGENT;", 1)),
    ("a User-Agent with no contact", "wiki_lookup.h",
     lambda t: t.replace('"NH-devices/1.0 (https://github.com/Nikguy321/wiphone-meshtastic) WiPhone"', '"NH-devices/1.0 WiPhone"', 1)),
    ("Google's roots for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.rootsPem = AI_WIKI_ROOTS_PEM;", "  w.rootsPem = AI_ROOTS_PEM;", 1)),
    ("X1 alone for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM", "#define AI_WIKI_ROOTS_PEM   ISRG_ROOT_X1_PEM", 1)),
    ("the whole sink for Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.cap = WIKI_RESPONSE_CAP;", "  w.cap = NET_SINK_CAP;", 1)),
    ("a 64 KB response cap", "wiki_lookup.h",
     lambda t: t.replace("#define WIKI_RESPONSE_CAP  (24u * 1024u)", "#define WIKI_RESPONSE_CAP  (64u * 1024u)", 1)),
    ("no total budget", "ai_net.cpp",
     lambda t: t.replace("  w.totalMs = AI_WIKI_TOTAL_MS;", "", 1)),
    ("a 60 s lookup", "ai_net.cpp",
     lambda t: t.replace("#define AI_WIKI_TOTAL_MS    20000u", "#define AI_WIKI_TOTAL_MS    60000u", 1)),
    ("Cancel ignored by the lookup", "ai_net.cpp",
     lambda t: t.replace("  w.cancel = &s_cancel;", "", 1)),
    ("a call ignored by the lookup", "ai_net.cpp",
     lambda t: t.replace("  w.stopFn = tileFetchCallRecent;", "", 1)),
    ("the url_context tool back", "gemini.cpp",
     lambda t: t.replace("  wChar(&w, ']');                                  // and never \"tools\"",
                         "  wChar(&w, ']');\n  wStr(&w, \",\\\"tools\\\":[{\\\"url_context\\\":{}}]\");", 1)),
    ("the chat folded with the background", "ai_net.cpp",
     lambda t: t.replace("geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, a,",
                         "geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_work->bg, a,", 1)),
    # round 3: "Retry if it times out", the lookup's rules, the round-2 review
    ("the ladder never told the setting", "ai_net.cpp",
     lambda t: t.replace("  l.retryTimeouts = q->retryTimeouts;", "", 1)),
    ("the setting not in the question", "ai_net.cpp",
     lambda t: t.replace("  s_req->retryTimeouts = aiRetryTimeouts();", "", 1)),
    ("the retry OFF when never set", "ai_net.cpp",
     lambda t: t.replace("p.getUChar(AI_PREFS_RETRY, 1)", "p.getUChar(AI_PREFS_RETRY, 0)", 1)),
    ("the setting in another namespace", "ai_net.cpp",
     lambda t: t.replace('#define AI_PREFS_NS     "wpmesh"', '#define AI_PREFS_NS     "aiprefs"', 1)),
    ("the ladder not told why there was no answer", "ai_net.cpp",
     lambda t: t.replace("code, net, rep, utcNow()", "code, GEM_NET_OK, rep, utcNow()", 1)),
    ("a Cancel retried as a timeout", "ai_net.cpp",
     lambda t: t.replace("    if (code <= 0 && s_cancel) {\n", "    if (false) {\n", 1)),
    ("the previous question only", "ai_net.cpp",
     lambda t: t.replace("wikiKeywords(q->question, earlier, nEarlier,", "wikiKeywords(q->question, earlier + nEarlier - 1, 1,", 1)),
    ("relevance skipped (no keywords to compose with)", "ai_net.cpp",
     lambda t: t.replace("wikiCompose(&s_work->wiki, s_work->kw,", "wikiCompose(&s_work->wiki, s_work->wikiUrl,", 1)),
    ("the background built past wikiCompose", "ai_net.cpp",
     lambda t: t.replace("  r->wikiPages = used;\n}", "  wikiBackground(&s_work->wiki, WIKI_BG_MAX, s_work->bg, sizeof(s_work->bg), NULL);\n  r->wikiPages = used;\n}", 1)),
    ("no heap bar before Gemini's handshake", "ai_net.cpp",
     lambda t: t.replace("  q.heapCheck = true;\n", "", 1)),
    ("NOMEM as a generic failure", "ai_net.cpp",          # Gemini's (D-175: aiRunChutes has its own, first)
     lambda t: t.replace("      if (code <= 0 && net == GEM_NET_NOMEM) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        geminiFailText(",
                         "      if (false) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        geminiFailText(", 1)),
    ("the budget not clamped before the request", "https_worker.cpp",
     lambda t: t.replace("          if (rest < hw) {\n            hw = rest;\n          }\n", "", 1)),
    ("the clamp gone (only the start's wait)", "https_worker.cpp",
     lambda t: t.replace("          http.setTimeout((uint16_t)hw);\n", "", 1)),
    ("a spent budget still sends", "https_worker.cpp",
     lambda t: t.replace("      if (connected && rest == 0) {", "      if (false) {", 1)),
    ("the guard not told the budget's end", "https_worker.cpp",
     lambda t: t.replace("            c->budgetEnds(t0 + q->totalMs);\n", "", 1)),
    ("a body cut at the budget called LOST", "https_worker.cpp",
     lambda t: t.replace("            if (c->timedOut()) {\n              r->net = GEM_NET_TIMEOUT;", "            if (c->timedOut()) {\n              r->net = GEM_NET_LOST;", 1)),
    ("the first byte disarms (no reading guard)", "https_worker.cpp",
     lambda t: t.replace("      m_answered = true;                           // the answer began", "      m_armed = false;                             // the answer began", 1)),
    ("the guard ignores a Cancel", "https_worker.cpp",
     lambda t: t.replace("const bool told = (m_cancel && *m_cancel) || (m_stopFn && m_stopFn());", "const bool told = (m_stopFn && m_stopFn());", 1)),
    ("the guard ignores a call", "https_worker.cpp",
     lambda t: t.replace("const bool told = (m_cancel && *m_cancel) || (m_stopFn && m_stopFn());", "const bool told = (m_cancel && *m_cancel);", 1)),
    ("the guard ignores the budget", "https_worker.cpp",
     lambda t: t.replace("const bool spent = m_hasEnd && (int32_t)(millis() - m_endMs) >= 0;", "const bool spent = false;", 1)),
    ("the guard does not stop the client", "https_worker.cpp",
     lambda t: t.replace("          m_timedOut = !told;\n          stop();\n", "          m_timedOut = !told;\n", 1)),
    # D-175: Chutes
    ("the Bearer to another host", "ai_net.cpp",
     lambda t: t.replace("  q.host = CHUTES_HOST;\n", "  q.host = GEM_HOST;\n", 1)),
    ("the Bearer to another URL", "ai_net.cpp",
     lambda t: t.replace("  q.url = CHUTES_URL;\n", "  q.url = \"https://example.com/v1/chat/completions\";\n", 1)),
    ("Chutes' host spelt wrong", "chutes.h",
     lambda t: t.replace('#define CHUTES_HOST          "llm.chutes.ai"', '#define CHUTES_HOST          "llm.chutes.ai.example.com"', 1)),
    ("the Chutes URL over plain HTTP", "chutes.h",
     lambda t: t.replace('"https://" CHUTES_HOST "/v1/chat/completions"', '"http://" CHUTES_HOST "/v1/chat/completions"', 1)),
    ("Google's roots for Chutes", "ai_net.cpp",
     lambda t: t.replace("  q.rootsPem = AI_CHUTES_ROOTS_PEM;", "  q.rootsPem = AI_ROOTS_PEM;", 1)),
    ("GoDaddy dropped from the set", "ai_net.cpp",
     lambda t: t.replace("#define AI_CHUTES_ROOTS_PEM GODADDY_ROOT_G2_PEM ISRG_ROOT_X1_PEM", "#define AI_CHUTES_ROOTS_PEM ISRG_ROOT_X1_PEM", 1)),
    ("Gemini's pair changed", "ai_net.cpp",
     lambda t: t.replace("AI_ROOTS_PEM[] = GTS_ROOT_R1_PEM GTS_ROOT_R4_PEM;", "AI_ROOTS_PEM[] = GTS_ROOT_R1_PEM;", 1)),
    ("the header not wiped", "ai_net.cpp",
     lambda t: t.replace("  int code = netHttps(&q, &r);\n  memset(s_work->auth, 0, sizeof(s_work->auth));\n", "  int code = netHttps(&q, &r);\n", 1)),
    ("the key in the URL, not a header", "ai_net.cpp",
     lambda t: t.replace('  q.hdrName[1] = "Authorization";', '  q.hdrName[1] = "X-Note";', 1)),
    ("the Bearer to Gemini too", "ai_net.cpp",
     lambda t: t.replace('  q.hdrName[1] = "x-goog-api-key";', '  q.hdrName[2] = "Authorization";\n  q.hdrValue[2] = s_work->auth;\n  q.hdrName[1] = "x-goog-api-key";', 1)),
    ("the Chutes key to Wikipedia", "ai_net.cpp",
     lambda t: t.replace("  w.userAgent = WIKI_USER_AGENT;", "  w.userAgent = WIKI_USER_AGENT;\n  w.hdrName[0] = \"Authorization\";\n  w.hdrValue[0] = s_work->auth;", 1)),
    ("Gemini's key in a Chutes request", "ai_net.cpp",
     lambda t: t.replace("chutesAuthHeader(s_req->chutes.key, s_work->auth,", "chutesAuthHeader(s_req->cfg.key, s_work->auth,", 1)),
    ("redirects followed", "https_worker.cpp",
     lambda t: t.replace("    http.setReuse(false);\n", "    http.setReuse(false);\n    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);\n", 1)),
    ("the Chutes wait not the preset's", "ai_net.cpp",
     lambda t: t.replace("  q.timeoutMs = p->waitMs;\n", "  q.timeoutMs = AI_TIMEOUT_MS;\n", 1)),
    ("no heap bar before Chutes' handshake", "ai_net.cpp",
     lambda t: t.replace("  q.heapCheck = true;                            // the bar before EVERY handshake (Gemini's rule)\n", "", 1)),
    ("a call does not end Chutes' wait", "ai_net.cpp",
     lambda t: t.replace("  q.stopFn = tileFetchCallRecent;\n  q.sink = netWorkerSink();\n  q.cap = AI_SINK_CAP;\n  NetHttpRes r;\n  int code = netHttps(&q, &r);\n  memset(s_work->auth",
                         "  q.sink = netWorkerSink();\n  q.cap = AI_SINK_CAP;\n  NetHttpRes r;\n  int code = netHttps(&q, &r);\n  memset(s_work->auth", 1)),
    ("Chutes' date from an untrusted clock", "ai_net.cpp",
     lambda t: t.replace("chutesSystemText(utcNow(),", "chutesSystemText((int64_t)ntpClock.getExactUtcTime(),", 1)),
    ("Chutes' background glued into the question", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, seed, s_body", "s_work->bg, q->question, s_work->sys, seed, s_body", 1)),
    ("the same seed every attempt", "ai_net.cpp",
     lambda t: t.replace("    seed = chutesSeed(esp_random(), seed);", "    seed = 42;", 1)),
    ("a Chutes Cancel retried as a timeout", "ai_net.cpp",
     lambda t: t.replace("      r->cancelled = true;                       // the user's word - never a timeout for rule 6\n      break;",
                         "      r->cancelled = true;                       // the user's word - never a timeout for rule 6", 1)),
    ("Chutes' ladder not told why", "ai_net.cpp",
     lambda t: t.replace("chutesLadderNext(&l, code, net, rep,", "chutesLadderNext(&l, code, GEM_NET_OK, rep,", 1)),
    ("a Chutes answer not scrubbed", "ai_net.cpp",
     lambda t: t.replace("        geminiScrub(r->answer, sizeof(r->answer), q->chutes.key);", "", 1)),
    ("a Chutes answer scrubbed of the wrong key", "ai_net.cpp",
     lambda t: t.replace("        geminiScrub(r->answer, sizeof(r->answer), q->chutes.key);", "        geminiScrub(r->answer, sizeof(r->answer), q->cfg.key);", 1)),
    ("the web's note hidden when nothing went (Chutes' sources tied to background again)", "ai_net.cpp",
     lambda t: t.replace("chutesAnswerShown(rep, l.cur, s_work->text, s_work->src[0] ? s_work->src : NULL,",
                         "chutesAnswerShown(rep, l.cur, s_work->text, s_work->bg[0] ? s_work->src : NULL,", 1)),
    ("Chutes looks Wikipedia up per attempt", "ai_net.cpp",
     lambda t: t.replace("    const uint32_t ta = millis();\n    size_t got = 0;\n    int net = GEM_NET_OK;\n    const int code = postOnceChutes(",
                         "    aiWikiLookup(q, r);\n    const uint32_t ta = millis();\n    size_t got = 0;\n    int net = GEM_NET_OK;\n    const int code = postOnceChutes(", 1)),
    ("Chutes asked before the lookup", "ai_net.cpp",
     lambda t: t.replace("  progSet(NULL, label, 0, 0);\n  aiWikiLookup(q, r);", "  if (chutes) {\n    aiRunChutes(q, r, ctx);\n  }\n  progSet(NULL, label, 0, 0);\n  aiWikiLookup(q, r);", 1)),
    ("Chutes' NOMEM as a generic failure", "ai_net.cpp",
     lambda t: t.replace("      if (code <= 0 && net == GEM_NET_NOMEM) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        chutesFailText(",
                         "      if (false) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        chutesFailText(", 1)),
    ("Chutes' ladder not told the setting", "ai_net.cpp",
     lambda t: t.replace("    s_work->cl.retryTimeouts = q->retryTimeouts;", "", 1)),
    ("the key in a log line", "ai_net.cpp",
     lambda t: t.replace('log_e("AI: chutes %s (%s)", s_m->ckey.line,', 'log_e("AI: chutes %s (%s)", s_ccfg->key,', 1)),
    ("the header on the WAITING screen", "ai_net.cpp",
     lambda t: t.replace("    progSet(l.note, label, 0, 0);", "    progSet(s_work->auth, label, 0, 0);", 1)),
    ("Key info holds the key", "ai_net.h",
     lambda t: t.replace("  int         preset;       // chutes.txt model= (-1 = none)", "  char        key[GEM_KEY_MAX];\n  int         preset;       // chutes.txt model= (-1 = none)", 1)),
    ("the app parses the key file", "app_ai.cpp",
     lambda t: t.replace("bool AiApp::choiceUsable() {\n", "bool AiApp::choiceUsable() {\n  ChutesConfig cc;\n  chutesParseConfig(\"\", 0, &cc);\n", 1)),
    ("`ai model` echoes the typed id", "serial_cmd.cpp",
     lambda t: t.replace('          say("ai: model gemini | v4flash | kimi | kimi_think | glm | default\\n");',
                         '          say("ai: model %s? gemini | v4flash | kimi | kimi_think | glm | default\\n", id);', 1)),
    ("the choice not in the question", "ai_net.cpp",
     lambda t: t.replace("  s_req->choice = choice;", "", 1)),
    ("the Gemini key rides along with a Chutes question", "ai_net.cpp",
     lambda t: t.replace("    memset(&s_req->cfg, 0, sizeof(s_req->cfg));\n    s_req->cfg.lookup = s_ccfg->lookup;",
                         "    memcpy(&s_req->cfg, s_cfg, sizeof(s_req->cfg));\n    s_req->cfg.lookup = s_ccfg->lookup;", 1)),
    ("the Chutes copy not wiped by the worker", "ai_net.cpp",
     lambda t: t.replace("  memset(s_req->chutes.key, 0, sizeof(s_req->chutes.key));   // D-175: the Chutes copy too\n", "", 1)),
    ("the Chutes copy not wiped on the fold", "ai_net.cpp",
     lambda t: t.replace("  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));\n  memset(s_req->chutes.key, 0, sizeof(s_req->chutes.key));\n"
                         "  memset(s_req->web.key", "  memset(s_req->cfg.key, 0, sizeof(s_req->cfg.key));\n  memset(s_req->web.key", 1)),
    ("the default rule bypassed (Chutes on without asking)", "ai_net.cpp",
     lambda t: t.replace("  return chutesChoiceResolve(s_m->modelStored, s_m->ckey.usable, s_m->ckey.preset);", "  return 0;", 1)),
    ("getString without isKey", "ai_net.cpp",
     lambda t: t.replace("    if (p.isKey(AI_PREFS_MODEL)) {\n      p.getString(AI_PREFS_MODEL, s_m->modelStored, sizeof(s_m->modelStored));\n    }",
                         "    p.getString(AI_PREFS_MODEL, s_m->modelStored, sizeof(s_m->modelStored));", 1)),
    ("an NVS key name too long", "ai_net.cpp",
     lambda t: t.replace('#define AI_PREFS_MODEL  "ai_model"', '#define AI_PREFS_MODEL  "ai_model_choice_v1"', 1)),
    ("any id stored", "ai_net.cpp",
     lambda t: t.replace("(choice != CHUTES_GEMINI && !chutesPreset(choice))", "(false)", 1)),
    ("a root copy of chutes.txt read", "ai_net.cpp",
     lambda t: t.replace("    there = SD.exists(AI_CHUTES_FILE);", "    there = SD.exists(AI_CHUTES_FILE) || SD.exists(\"/chutes.txt\");", 1)),
    # the D-175 review (2026-10-18): each leak, trap and gap it found, re-made
    ("the key in a log line from the parse's own copy (c->key)", "ai_net.cpp",
     lambda t: t.replace("        const int rc = chutesParseConfig(buf, (size_t)n, c);\n",
                         "        const int rc = chutesParseConfig(buf, (size_t)n, c);\n        log_e(\"AI: chutes key %s\", c->key);\n", 1)),
    ("Gemini's key in a log line from its parse", "ai_net.cpp",
     lambda t: t.replace("        const int rc = geminiParseConfig(buf, (size_t)n, c);\n",
                         "        const int rc = geminiParseConfig(buf, (size_t)n, c);\n        log_e(\"AI: key %s\", c->key);\n", 1)),
    ("the key file's bytes to the console", "ai_net.cpp",
     lambda t: t.replace("        const int rc = chutesParseConfig(buf, (size_t)n, c);\n",
                         "        const int rc = chutesParseConfig(buf, (size_t)n, c);\n        Serial.write((const uint8_t*)buf, n);\n", 1)),
    ("the whole parse to the console", "ai_net.cpp",
     lambda t: t.replace("        const int rc = chutesParseConfig(buf, (size_t)n, c);\n",
                         "        const int rc = chutesParseConfig(buf, (size_t)n, c);\n        Serial.write((const uint8_t*)c, sizeof(*c));\n", 1)),
    ("the key on Key info", "ai_net.cpp",
     lambda t: t.replace("        chutesConfigLine(c, rc, s_m->ckey.line, sizeof(s_m->ckey.line));",
                         "        strlcpy(s_m->ckey.line, c->key, sizeof(s_m->ckey.line));", 1)),
    ("Key info's line from the file's bytes", "ai_net.cpp",
     lambda t: t.replace("        chutesConfigLine(c, rc, s_m->ckey.line, sizeof(s_m->ckey.line));",
                         "        strlcpy(s_m->ckey.line, buf, sizeof(s_m->ckey.line));", 1)),
    ("the key as the WAITING screen's label", "ai_net.cpp",
     lambda t: t.replace("    strlcpy(s_m->prog.label, chutesChoiceLabel(choice), sizeof(s_m->prog.label));",
                         "    strlcpy(s_m->prog.label, s_ccfg->key, sizeof(s_m->prog.label));", 1)),
    ("the key as the countdown's short name", "ai_net.cpp",
     lambda t: t.replace("  progShort(p->shortName);\n", "  progShort(q->chutes.key);\n", 1)),
    ("the key as the failure (drawn, saved, `ai`)", "ai_net.cpp",
     lambda t: t.replace("    chutesFailText(0, GEM_NET_NOMEM, NULL, NULL, q->chutes.key, r->fail, sizeof(r->fail));",
                         "    strlcpy(r->fail, q->chutes.key, sizeof(r->fail));", 1)),
    ("the header kept in a second buffer", "ai_net.cpp",
     lambda t: t.replace("  q.hdrValue[1] = s_work->auth;\n", "  q.hdrValue[1] = s_work->auth;\n  strlcpy(s_work->stage, s_work->auth, sizeof(s_work->stage));\n", 1)),
    ("Chutes' raw body as the failure (a 402 names a wallet)", "ai_net.cpp",
     lambda t: t.replace("        chutesFailText(code, net, rep, &l, q->chutes.key, r->fail, sizeof(r->fail));\n",
                         "        chutesFailText(code, net, rep, &l, q->chutes.key, r->fail, sizeof(r->fail));\n"
                         "        if (code > 0) strlcpy(r->fail, netWorkerSink(), sizeof(r->fail));\n", 1)),
    ("Chutes' raw body in the log", "ai_net.cpp",
     lambda t: t.replace("    if (step == GEM_STEP_FAIL) {\n      if (code <= 0 && net == GEM_NET_NOMEM) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        chutesFailText(",
                         "    if (step == GEM_STEP_FAIL) {\n      log_e(\"AI: chutes HTTP %d: %.120s\", code, netWorkerSink());\n"
                         "      if (code <= 0 && net == GEM_NET_NOMEM) {\n        lowMemText(r->fail, sizeof(r->fail));"
                         "    // the heap bar (q.heapCheck) or the handshake's alloc\n      } else {\n        chutesFailText(", 1)),
    ("the raw body aliased past the rule", "ai_net.cpp",
     lambda t: t.replace("    uint32_t delayMs = 0;\n    const char* stage = NULL;\n    const int step = chutesLadderNext(",
                         "    const char* raw = netWorkerSink();\n    uint32_t delayMs = 0;\n    const char* stage = NULL;\n    const int step = chutesLadderNext(", 1)),
    ("the raw text as the failure", "ai_net.cpp",
     lambda t: t.replace("        chutesNoTextMessage(rep, l.cur, r->fail, sizeof(r->fail));",
                         "        strlcpy(r->fail, s_work->text, sizeof(r->fail));", 1)),
    ("the key sent as the question", "ai_net.cpp",
     lambda t: t.replace("q->question, s_work->bg, s_work->sys, seed, s_body", "q->chutes.key, s_work->bg, s_work->sys, seed, s_body", 1)),
    ("gemini.txt read past Gemini's key rule (a cpk_ key to Google)", "ai_net.cpp",
     lambda t: t.replace("        const int rc = geminiParseConfig(buf, (size_t)n, c);", "        const int rc = geminiReadKeyFile(buf, (size_t)n, c);", 1)),
    ("chutes.txt read through Gemini's refusal", "chutes.cpp",
     lambda t: t.replace("  const int rc = geminiReadKeyFile(text, len, g);", "  const int rc = geminiParseConfig(text, len, g);", 1)),
    ("the reload's parse wiped by a memset (dropped)", "ai_net.cpp",
     lambda t: t.replace("        geminiWipe(c, sizeof(*c));\n        free(c);", "        memset(c, 0, sizeof(*c));\n        free(c);", 1)),
    ("the reload's buffer freed unwiped", "ai_net.cpp",
     lambda t: t.replace("      geminiWipe(buf, AI_KEY_READ_MAX);\n      free(buf);", "      free(buf);", 1)),
    ("Gemini's parse freed unwiped", "ai_net.cpp",
     lambda t: _nth(t, "        geminiWipe(c, sizeof(*c));\n        free(c);", "        free(c);", 2)),
    ("Gemini's buffer wiped by a memset (dropped)", "ai_net.cpp",
     lambda t: _nth(t, "      geminiWipe(buf, AI_KEY_READ_MAX);\n      free(buf);", "      memset(buf, 0, AI_KEY_READ_MAX);\n      free(buf);", 2)),
    ("Chutes' reply freed unwiped", "ai_net.cpp",
     lambda t: t.replace("  geminiWipe(rep, sizeof(*rep));\n  free(rep);\n}", "  free(rep);\n}", 1)),
    ("Gemini's reply wiped by a memset (dropped)", "ai_net.cpp",
     lambda t: t.replace("  geminiWipe(rep, sizeof(*rep));          // Google's message may quote what was sent",
                         "  memset(rep, 0, sizeof(*rep));           // Google's message may quote what was sent", 1)),
    ("geminiWipe a plain memset", "gemini.cpp",
     lambda t: t.replace("  volatile unsigned char* v = (volatile unsigned char*)p;\n  while (v && n--) {\n    *v++ = 0;\n  }",
                         "  if (p) memset(p, 0, n);", 1)),
    ("a Chutes question goes on to Gemini (no return)", "ai_net.cpp",
     lambda t: t.replace("    aiRunChutes(q, r, ctx);                 // D-175: its requests; Gemini's below are untouched\n    r->ms = millis() - t0;\n    return;\n",
                         "    aiRunChutes(q, r, ctx);                 // D-175: its requests; Gemini's below are untouched\n    r->ms = millis() - t0;\n", 1)),
    ("a failed Chutes question goes on to Gemini", "ai_net.cpp",
     lambda t: t.replace("    aiRunChutes(q, r, ctx);                 // D-175: its requests; Gemini's below are untouched\n    r->ms = millis() - t0;\n    return;\n",
                         "    aiRunChutes(q, r, ctx);                 // D-175: its requests; Gemini's below are untouched\n    r->ms = millis() - t0;\n    if (r->ok) return;\n", 1)),
    ("an answer never marks Chutes as answered", "ai_net.cpp",
     lambda t: t.replace("      s_m->chutesAnswered = true;                // a 200: the key works - a later 429 is load, not the key\n", "", 1)),
    ("every question is the boot's first", "ai_net.cpp",
     lambda t: t.replace("s_m->chutesGone, !s_m->chutesAnswered, millis()", "s_m->chutesGone, true, millis()", 1)),
    ("the gone presets forgotten after the question", "ai_net.cpp",
     lambda t: t.replace("  s_m->chutesGone = l.goneMask;                  // a model gone this boot is skipped by the next question\n", "", 1)),
    ("the gone presets not handed to the next question", "ai_net.cpp",
     lambda t: t.replace("s_m->chutesGone, !s_m->chutesAnswered, millis()", "0, !s_m->chutesAnswered, millis()", 1)),
    ("the reply parsed as another preset's", "ai_net.cpp",
     lambda t: t.replace("chutesParseReply(l.cur, netWorkerSink(),", "chutesParseReply(0, netWorkerSink(),", 1)),
    ("a missed chutes.txt read keeps a key never read", "ai_net.cpp",
     lambda t: t.replace("  if (!there && s_cHaveGood && s_cKeyMisses < 2) {", "  if (!there && s_cKeyMisses < 2) {", 1)),
    ("an unreadable chutes.txt usable with no good read", "ai_net.cpp",
     lambda t: t.replace("      s_m->ckey.usable = s_cHaveGood;\n      if (s_cHaveGood) {", "      s_m->ckey.usable = true;\n      if (s_cHaveGood) {", 1)),
    ("a refused chutes.txt keeps the key it replaced", "ai_net.cpp",
     lambda t: t.replace("          memset(s_ccfg, 0, sizeof(*s_ccfg));\n          s_ccfg->preset = -1;\n          s_cHaveGood = false;\n",
                         "          s_ccfg->preset = -1;\n", 1)),
    ("a missed gemini.txt read keeps a key never read", "ai_net.cpp",
     lambda t: t.replace("  if (!path && s_haveGood && s_keyMisses < 2) {", "  if (!path && s_keyMisses < 2) {", 1)),
    ("either key makes every choice usable", "ai_net.cpp",
     lambda t: t.replace("  return chutesPreset(choice) ? s_m->ckey.usable : s_m->key.usable;", "  return s_m->ckey.usable || s_m->key.usable;", 1)),
    ("aiAsk trusts a latched-off Chutes key", "ai_net.cpp",
     lambda t: t.replace("(s_m->ckey.usable && s_ccfg && s_cHaveGood)", "(s_m->ckey.usable)", 1)),
    ("Root YR dropped from Chutes' set", "ai_net.cpp",
     lambda t: t.replace("ISRG_ROOT_X2_PEM ISRG_ROOT_YR_PEM ISRG_ROOT_YE_PEM \\\n", "ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM \\\n", 1)),
    ("Root YE dropped from Chutes' set", "ai_net.cpp",
     lambda t: t.replace("ISRG_ROOT_X2_PEM ISRG_ROOT_YR_PEM ISRG_ROOT_YE_PEM \\\n", "ISRG_ROOT_X2_PEM ISRG_ROOT_YR_PEM \\\n", 1)),
    ("Key info's Back closes the app with a key there", "app_ai.cpp",
     lambda t: t.replace("      if (!key.usable && !ckey.usable && (!aiChat() || aiChat()->n == 0)) {",
                         "      if (!choiceUsable() && (!aiChat() || aiChat()->n == 0)) {", 1)),
    ("Key info's Back asks a stale Chutes key", "app_ai.cpp",
     lambda t: t.replace("      aiKeyInfo(&key);\n      aiChutesKeyInfo(&ckey);\n      aiWebKeyInfo(&wkey);\n      /* Out only",
                         "      aiKeyInfo(&key);\n      aiWebKeyInfo(&wkey);\n      /* Out only", 1)),
    ("Key info's Model row does nothing", "app_ai.cpp",
     lambda t: t.replace("      if (k == ROW_MODEL) {\n        aiKeyReload();                    // the rows say which key files are there now\n        enterState(AI_MODELS);\n        return REDRAW_ALL;\n      }\n      if (k == ROW_UPLOAD) {",
                         "      if (k == ROW_UPLOAD) {", 1)),
    ("no Model row on Key info", "app_ai.cpp",
     lambda t: t.replace("    menu->addOption(line, ROW_MODEL);\n  }\n", "  }\n", 1)),
    ("the uploader's line for either key", "app_ai.cpp",
     lambda t: t.replace("    const bool found = choiceUsable();", "    const bool found = key.usable || ckey.usable;", 1)),
    ("OK on the model in use pins it", "app_ai.cpp",
     lambda t: t.replace("      if (c != aiModelChoice()) {\n        aiSetModelChoice(c);\n      }", "      aiSetModelChoice(c);", 1)),
    ("a stray chutes.txt cloned", CLONE,
     lambda t: t.replace("--exclude '[Cc][Hh][Uu][Tt][Ee][Ss].[Tt][Xx][Tt]' ", "", 1)),
    ("chutes.txt counted on both sides", CLONE,
     lambda t: t.replace(" ! -iname 'chutes.txt'", "", 1)),
    ("a stray gemini.txt cloned (a case-SENSITIVE exclude)", CLONE,
     lambda t: t.replace("--exclude '[Gg][Ee][Mm][Ii][Nn][Ii].[Tt][Xx][Tt]'", "--exclude 'gemini.txt'", 1)),
    ("the API Keys folder cloned", CLONE,
     lambda t: t.replace("--exclude '[Aa][Pp][Ii] [Kk][Ee][Yy][Ss]/' ", "", 1)),
    ("the usage line forgets chutes.txt", CLONE,
     lambda t: t.replace("any 'API Keys' folder, gemini.txt, chutes.txt and tavily.txt are left out.",
                         "any 'API Keys' folder, gemini.txt and tavily.txt are left out.", 1)),
    ("a new key file the clone script does not know", "files_paths.h",
     lambda t: t.replace('strncasecmp(p, "tavily.txt", 10) == 0)', 'strncasecmp(p, "tavily.txt", 10) == 0 || strncasecmp(p, "openai.txt", 10) == 0)', 1)),
    # the X4's review (2026-10-19), brought back: contract 21 and contract 16's l.cur
    ("the answer's declined note names the preset ASKED", "ai_net.cpp",
     lambda t: t.replace("chutesAnswerShown(rep, l.cur, s_work->text,", "chutesAnswerShown(rep, q->choice, s_work->text,", 1)),
    # the WiPhone's review (2026-10-19): "any text" is what the screen shows, judged before the ladder
    ("the answer judged after the ladder (an emoji alone + abort: 'sent an empty answer')", "ai_net.cpp",
     lambda t: _move_shown_after_ladder(t)),
    ("the ladder reads the parser's TEXT (the answer built after it, as before)", "ai_net.cpp",
     lambda t: t.replace(_SHOWN_CALL, "", 1).replace(
         "      if (rep->kind == CHUTES_REPLY_TEXT && r->answer[0]) {",
         "      if (rep->kind == CHUTES_REPLY_TEXT &&\n          chutesAnswerText(rep, l.cur, s_work->text, "
         "s_work->bg[0] ? s_work->src : NULL, r->answer,\n                           sizeof(r->answer)) > 0) {", 1)),
    ("a DONE with a TEXT the screen does not show called an answer", "ai_net.cpp",
     lambda t: t.replace("      if (rep->kind == CHUTES_REPLY_TEXT && r->answer[0]) {", "      if (r->answer[0] || true) {", 1)),
    ("the raw text into a second answer builder", "ai_net.cpp",
     lambda t: t.replace("        chutesAnswerLabel(&l, r->label, sizeof(r->label));",
                         "        chutesAnswerLabel(&l, r->label, sizeof(r->label));\n"
                         "        chutesAnswerText(rep, l.cur, s_work->text, NULL, r->answer, sizeof(r->answer));", 1)),
    ("a topic spans the providers (aiAsk asks the provider-blind rule)", "ai_net.cpp",
     lambda t: t.replace("  s_req->topic = chutesTopicDue(chat, s_req->askedUtc, choice, s_m->topicLast);",
                         "  s_req->topic = geminiChatTopicDue(chat, s_req->askedUtc);", 1)),
    ("aiAsk's topic told no boot memory", "ai_net.cpp",
     lambda t: t.replace("chutesTopicDue(chat, s_req->askedUtc, choice, s_m->topicLast);",
                         "chutesTopicDue(chat, s_req->askedUtc, choice, CHUTES_BY_UNKNOWN);", 1)),
    ("this boot's last provider never kept", "ai_net.cpp",
     lambda t: t.replace("  s_m->topicLast = chutesChoiceProvider(choice);  // asked: nothing below refuses it\n", "", 1)),
    ("this boot's last provider kept before a refusal", "ai_net.cpp",
     lambda t: t.replace("  s_m->topicLast = chutesChoiceProvider(choice);  // asked: nothing below refuses it\n", "", 1).replace(
         "  const int choice = aiModelChoice();\n  const bool chutes = choice != CHUTES_GEMINI;\n",
         "  const int choice = aiModelChoice();\n  const bool chutes = choice != CHUTES_GEMINI;\n  s_m->topicLast = chutesChoiceProvider(choice);\n", 1)),
    ("the context sent with a question that starts a topic", "ai_net.cpp",
     lambda t: t.replace("  s_req->nCtx = 0;\n  if (!s_req->topic) {", "  s_req->nCtx = 0;\n  {", 1)),
    ("the boot's last provider starts as Gemini (calloc's 0)", "ai_net.cpp",
     lambda t: t.replace("      s_m->topicLast = CHUTES_BY_UNKNOWN;   // nothing asked this boot\n", "", 1)),
    ("aiTopicDue provider-blind", "ai_net.cpp",
     lambda t: t.replace("  const bool due = chutesTopicDue(c, now, aiModelChoice(), s_m->topicLast);",
                         "  const bool due = geminiChatTopicDue(c, now);", 1)),
    ("aiTopicDue asks for Gemini whatever is in use", "ai_net.cpp",
     lambda t: t.replace("chutesTopicDue(c, now, aiModelChoice(), s_m->topicLast);", "chutesTopicDue(c, now, CHUTES_GEMINI, s_m->topicLast);", 1)),
    ("aiTopicDue's 'only the provider' for every topic", "ai_net.cpp",
     lambda t: t.replace("    *otherProvider = due && !geminiChatTopicDue(c, now);", "    *otherProvider = due;", 1)),
    ("`ai` says the provider-blind next question", "ai_net.cpp",
     lambda t: t.replace("    const bool topic = aiTopicDue(&other);", "    const bool topic = geminiChatTopicDue(c, utcNow());", 1)),
    ("Menu > AI never says the next question starts a topic", "app_ai.cpp",
     lambda t: t.replace("    menu->addNoteWrapped(CHUTES_TOPIC_SWITCH);\n", "", 1)),
    ("Menu > AI says it after every New topic (the public phone's menu changed)", "app_ai.cpp",
     lambda t: t.replace("  if (aiTopicDue(&otherProvider) && otherProvider) {", "  if (aiTopicDue(&otherProvider)) {", 1)),
    ("`ai model` silent about the new topic", "serial_cmd.cpp",
     lambda t: t.replace("      if (aiTopicDue(&otherProvider) && otherProvider) {\n        say(", "      if (false) {\n        say(", 1)),
    ("the app decides a topic provider-blind", "app_ai.cpp",
     lambda t: t.replace("  bool otherProvider = false;\n", "  bool otherProvider = geminiChatTopicDue(aiChat(), 0);\n", 1)),
    # ── D-176, the web search: contracts 22-25 (26-27 below: its review) ──
    ("the Tavily Bearer to another host", "ai_net.cpp",
     lambda t: t.replace("  w.host = WEB_HOST;", "  w.host = CHUTES_HOST;", 1)),
    ("the Tavily Bearer to another URL", "ai_net.cpp",
     lambda t: t.replace("  w.url = WEB_URL;", "  w.url = s_work->webBody;", 1)),
    ("WEB_HOST not Tavily's", "websearch.h",
     lambda t: t.replace('#define WEB_HOST            "api.tavily.com"', '#define WEB_HOST            "api.tavily.com.example.net"', 1)),
    ("the search over plain HTTP", "websearch.h",
     lambda t: t.replace('#define WEB_URL             "https://" WEB_HOST "/search"', '#define WEB_URL             "http://" WEB_HOST "/search"', 1)),
    ("the Tavily key sent as the body", "ai_net.cpp",
     lambda t: t.replace("  w.body = (const uint8_t*)s_work->webBody;", "  w.body = (const uint8_t*)s_work->webAuth;", 1)),
    ("the Tavily key as a header of another name", "ai_net.cpp",
     lambda t: t.replace('  w.hdrName[1] = "Authorization";', '  w.hdrName[1] = "X-Api-Key";', 1)),
    ("the Tavily header never wiped", "ai_net.cpp",
     lambda t: t.replace("  const int code = netHttps(&w, &res);\n  memset(s_work->webAuth, 0, sizeof(s_work->webAuth));\n",
                         "  const int code = netHttps(&w, &res);\n", 1)),
    ("the Tavily header built from the Chutes key", "ai_net.cpp",
     lambda t: t.replace("webAuthHeader(q->web.key, s_work->webAuth,", "webAuthHeader(q->chutes.key, s_work->webAuth,", 1)),
    ("the search retried", "ai_net.cpp",
     lambda t: t.replace("  const int code = netHttps(&w, &res);",
                         "  int code = 0;\n  for (int k = 0; k < 2 && code != 200; k++) {\n    code = netHttps(&w, &res);\n  }", 1)),
    ("a second search request", "ai_net.cpp",
     lambda t: t.replace("  const int code = netHttps(&w, &res);", "  netHttps(&w, &res);\n  const int code = netHttps(&w, &res);", 1)),
    ("the Tavily roots without Starfield", "ai_net.cpp",
     lambda t: t.replace("#define AI_TAVILY_ROOTS_PEM AMAZON_ROOT_CA1_PEM STARFIELD_SERVICES_ROOT_G2_PEM", "#define AI_TAVILY_ROOTS_PEM AMAZON_ROOT_CA1_PEM", 1)),
    ("the Tavily roots widened to ISRG", "ai_net.cpp",
     lambda t: t.replace("#define AI_TAVILY_ROOTS_PEM AMAZON_ROOT_CA1_PEM STARFIELD_SERVICES_ROOT_G2_PEM",
                         "#define AI_TAVILY_ROOTS_PEM AMAZON_ROOT_CA1_PEM STARFIELD_SERVICES_ROOT_G2_PEM ISRG_ROOT_X1_PEM", 1)),
    ("the search pinned to Chutes' roots", "ai_net.cpp",
     lambda t: t.replace("  w.rootsPem = AI_TAVILY_ROOTS_PEM;", "  w.rootsPem = AI_CHUTES_ROOTS_PEM;", 1)),
    ("a root fingerprint dropped from amazon_roots.h", "amazon_roots.h",
     lambda t: t.replace("56:8D:69:05:A2:C8:87:08:A4:B3:02:51:90:ED:CF:ED:B1:97:4A:60:6A:13:C6:E5:29:0F:CB:2A:E6:3E:DA:B5", "(see the keychain)")),
    ("the raw reply kept", "ai_net.cpp",
     lambda t: t.replace("  w.cap = 0;  ", "  w.cap = AI_SINK_CAP;  ", 1)),
    ("the reply never parsed as it arrives", "ai_net.cpp",
     lambda t: t.replace("  w.tap = webTap;\n", "", 1)),
    ("the tap keeps the body", "ai_net.cpp",
     lambda t: t.replace("  webParseFeed((WebParse*)ctx, p, n);\n}", "  webParseFeed((WebParse*)ctx, p, n);\n  memcpy(s_work->web, p, n < WEB_BLOCK_MAX ? n : WEB_BLOCK_MAX);\n}", 1)),
    ("Tavily's 3 s budget gone (the AI's 150 s)", "ai_net.cpp",
     lambda t: t.replace("  w.timeoutMs = WEB_ANSWER_MS;", "  w.timeoutMs = AI_TIMEOUT_MS;", 1)),
    ("WEB_ANSWER_MS 30 s", "websearch.h",
     lambda t: t.replace("#define WEB_ANSWER_MS       6000u", "#define WEB_ANSWER_MS       30000u", 1)),
    ("no total budget for the search", "ai_net.cpp",
     lambda t: t.replace("  w.totalMs = AI_WEB_TOTAL_MS;", "", 1)),
    ("the search's handshake under 20 s", "ai_net.cpp",
     lambda t: t.replace("#define AI_WEB_CONNECT_MS   6000u", "#define AI_WEB_CONNECT_MS   20000u", 1)),
    ("the search's budget more than the two", "ai_net.cpp",
     lambda t: t.replace("#define AI_WEB_TOTAL_MS     (AI_WEB_CONNECT_MS + WEB_ANSWER_MS)", "#define AI_WEB_TOTAL_MS     30000u", 1)),
    ("Cancel does not end the search", "ai_net.cpp",
     lambda t: t.replace("  w.cancel = &s_cancel;\n  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)\n  w.sink = netWorkerSink();\n  w.cap = 0;",
                         "  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)\n  w.sink = netWorkerSink();\n  w.cap = 0;", 1)),
    ("a call does not end the search", "ai_net.cpp",
     lambda t: t.replace("  w.stopFn = tileFetchCallRecent;                  // a call rang in: stop (aiRun then says so)\n  w.sink = netWorkerSink();\n  w.cap = 0;",
                         "  w.sink = netWorkerSink();\n  w.cap = 0;", 1)),
    ("the search's handshake under no heap bar", "ai_net.cpp",
     lambda t: _nth(t, "  w.heapCheck = true;                              // the bar again before THIS handshake\n", "", 2)),
    ("the lookup's handshake under no heap bar", "ai_net.cpp",
     lambda t: _nth(t, "  w.heapCheck = true;                              // the bar again before THIS handshake\n", "", 1)),
    ("the Tavily key to Chutes too", "ai_net.cpp",
     lambda t: t.replace("  q.hdrValue[1] = s_work->auth;", "  q.hdrValue[1] = s_work->auth;\n  q.hdrName[2] = \"X-Tavily\";\n  q.hdrValue[2] = s_req->web.key;", 1)),
    ("the Tavily key to Google too", "ai_net.cpp",
     lambda t: t.replace('  q.hdrValue[1] = s_req->cfg.key;', '  q.hdrValue[1] = s_req->cfg.key;\n  q.hdrName[2] = "X-Tavily";\n  q.hdrValue[2] = s_req->web.key;', 1)),
    ("the Gemini key in the search", "ai_net.cpp",
     lambda t: t.replace("  w.hdrName[0] = \"Content-Type\";\n  w.hdrValue[0] = \"application/json\";\n  w.hdrName[1] = \"Authorization\";",
                         "  w.hdrName[2] = \"x-goog-api-key\";\n  w.hdrValue[2] = q->cfg.key;\n  w.hdrName[0] = \"Content-Type\";\n  w.hdrValue[0] = \"application/json\";\n  w.hdrName[1] = \"Authorization\";", 1)),
    ("the chat's answers in the search body", "ai_net.cpp",
     lambda t: t.replace("webBuildRequest(q->question, q->webDec.topic,", "webBuildRequest(q->ctxA[0], q->webDec.topic,", 1)),
    ("the Tavily key in a log line", "ai_net.cpp",
     lambda t: t.replace('    log_e("AI: tavily %s (%s)", s_m->wkey.line,', '    log_e("AI: tavily %s (%s)", s_wcfg->key,', 1)),
    ("the Tavily key in `ai`", "ai_net.cpp",
     lambda t: t.replace('  snprintf(l, sizeof(l), "    tavily %s, file %s, roots built-in Amazon Root CA 1 + Starfield G2", s_m->wkey.line,',
                         '  snprintf(l, sizeof(l), "    tavily %s, file %s, roots built-in Amazon Root CA 1 + Starfield G2", s_req->web.key,', 1)),
    ("AiWebKey carries the key", "ai_net.h",
     lambda t: t.replace("  char        line[AI_KEY_LINE_MAX];   // webConfigLine: \"key: set (41 chars)\"\n} AiWebKey;",
                         "  char        line[AI_KEY_LINE_MAX];   // webConfigLine: \"key: set (41 chars)\"\n  char        key[208];\n} AiWebKey;", 1)),
    ("the app parses tavily.txt itself", "app_ai.cpp",
     lambda t: t.replace("  aiWebKeyInfo(&wkey);\n  aiLoopTick();", "  aiWebKeyInfo(&wkey);\n  { WebConfig wc; webParseConfig(\"\", 0, &wc); }\n  aiLoopTick();", 1)),
    ("`ai web` echoes what was typed", "serial_cmd.cpp",
     lambda t: t.replace('        say("ai web: ai web [off|auto|always] | ai web test <question>\\n");', '        say("ai web: no mode \'%s\'\\n", a);', 1)),
    ("`ai web test` echoes the question", "serial_cmd.cpp",
     lambda t: t.replace('          say("ai web: NOT searched - %s\\n", why);', '          say("ai web: NOT searched - %s (%s)\\n", why, q);', 1)),
    ("the Tavily key copied into every question", "ai_net.cpp",
     lambda t: t.replace("  if (s_req->webDec.search) {\n    memcpy(&s_req->web, s_wcfg, sizeof(s_req->web));",
                         "  if (true) {\n    memcpy(&s_req->web, s_wcfg, sizeof(s_req->web));", 1)),
    ("the decision told any read of tavily.txt is good", "ai_net.cpp",
     lambda t: t.replace("  webDecide(aiWebMode(), aiWebUsable(), forceWeb,", "  webDecide(aiWebMode(), s_m->wkey.state != AI_KEY_NONE, forceWeb,", 1)),
    ("the per-question force lost", "ai_net.cpp",
     lambda t: t.replace("  webDecide(aiWebMode(), aiWebUsable(), forceWeb,", "  webDecide(aiWebMode(), aiWebUsable(), false,", 1)),
    ("the cap judged on nothing (a reboot resets it)", "ai_net.cpp",
     lambda t: t.replace("webDayAutoUsed(&s_m->day, webToday()),\n            &s_req->webDec);", "0,\n            &s_req->webDec);", 1)),
    ("the cap read before NVS is", "ai_net.cpp",
     lambda t: t.replace("  dayLoad();\n  webDecide(aiWebMode()", "  webDecide(aiWebMode()", 1)),
    ("aiWebUsable without a good read", "ai_net.cpp",
     lambda t: t.replace("  return memOk() && s_m->wkey.usable && s_wcfg && s_wHaveGood;", "  return memOk() && s_m->wkey.usable;", 1)),
    ("a bench search under Off", "ai_net.cpp",
     lambda t: t.replace("  if (aiWebMode() == WEB_OFF) {\n    strlcpy(why, \"Web search is Off", "  if (false) {\n    strlcpy(why, \"Web search is Off", 1)),
    ("a bench search carries the Gemini key", "ai_net.cpp",
     lambda t: t.replace("  s_req->choice = CHUTES_GEMINI;\n  memset(&s_req->cfg, 0, sizeof(s_req->cfg));\n",
                         "  s_req->choice = CHUTES_GEMINI;\n  memcpy(&s_req->cfg, s_cfg, sizeof(s_req->cfg));\n", 1)),
    ("the bench search runs the ladder too", "ai_net.cpp",
     lambda t: t.replace("    aiWebSearch(q, r);\n    r->cancelled = s_cancel;\n    r->ms = millis() - t0;\n    return;\n",
                         "    aiWebSearch(q, r);\n    r->cancelled = s_cancel;\n    r->ms = millis() - t0;\n", 1)),
    ("the search before the Wikipedia lookup", "ai_net.cpp",
     lambda t: _move_search_before_lookup(t)),
    ("the search after the provider (Chutes)", "ai_net.cpp",
     lambda t: t.replace("  if (chutes) {\n    aiRunChutes(q, r, ctx);", "  if (chutes) {\n    aiRunChutes(q, r, ctx);\n    aiWebSearch(q, r);", 1)),
    ("no search for a question", "ai_net.cpp",
     lambda t: _drop_main_search(t)),
    ("a search never counted", "ai_net.cpp",
     lambda t: t.replace("      webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);\n", "", 1)),
    ("the counts never saved (a reboot resets the cap)", "ai_net.cpp",
     lambda t: t.replace("      webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);\n      daySave();\n",
                         "      webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);\n", 1)),
    ("every search counted as automatic", "ai_net.cpp",
     lambda t: t.replace("webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);", "webDayCount(&s_m->day, webToday(), true);", 1)),
    ("the bench search returns before it is counted", "ai_net.cpp",
     lambda t: t.replace("  if (s_res->webAsked && memOk()) {\n    dayLoad();",
                         "  if (s_req->webOnly) {\n    s_done = false;\n    return;\n  }\n  if (s_res->webAsked && memOk()) {\n    dayLoad();", 1)),
    ("the day from an untrusted clock", "ai_net.cpp",
     lambda t: t.replace("  return webLocalDay(utcNow(), tzNow());", "  return webLocalDay((int64_t)ntpClock.getExactUtcTime(), tzNow());", 1)),
    ("an NVS name past 15 characters", "ai_net.cpp",
     lambda t: t.replace('#define AI_PREFS_WEBD   "ai_webd"', '#define AI_PREFS_WEBD   "ai_web_day_counts"', 1)),
    ("ai_web read without isKey", "ai_net.cpp",
     lambda t: t.replace("    if (p.isKey(AI_PREFS_WEB)) {\n      p.getString(AI_PREFS_WEB, s_m->webStored, sizeof(s_m->webStored));\n    }",
                         "    p.getString(AI_PREFS_WEB, s_m->webStored, sizeof(s_m->webStored));", 1)),
    ("ai_web stores anything", "ai_net.cpp",
     lambda t: t.replace("  if (!memOk() || mode < 0 || mode >= WEB_MODES) {\n    return;                                        // the allowlist",
                         "  if (!memOk()) {\n    return;                                        // the allowlist", 1)),
    ("the Tavily copy not wiped by the job", "ai_net.cpp",
     lambda t: t.replace("  memset(s_req->web.key, 0, sizeof(s_req->web.key));         // D-176: the Tavily copy\n", "", 1)),
    ("the Tavily header not wiped by the job", "ai_net.cpp",
     lambda t: t.replace("  memset(s_work->webAuth, 0, sizeof(s_work->webAuth));       // ...and its header (wiped after its request already)\n", "", 1)),
    ("the Tavily copy not wiped on the fold", "ai_net.cpp",
     lambda t: t.replace("  memset(s_req->web.key, 0, sizeof(s_req->web.key));        // D-176\n", "", 1)),
    ("tavily.txt read from the card's root too", "ai_net.cpp",
     lambda t: t.replace("    there = SD.exists(AI_TAVILY_FILE);", "    there = SD.exists(AI_TAVILY_FILE) || SD.exists(\"/tavily.txt\");", 1)),
    ("tavily.txt parsed past the tvly- rule", "ai_net.cpp",
     lambda t: t.replace("        const int rc = webParseConfig(buf, (size_t)n, c);", "        const int rc = geminiReadKeyFile(buf, (size_t)n, &c->scratch);", 1)),
    ("tavily.txt not read with the others", "ai_net.cpp",
     lambda t: t.replace("  webKeyReload();                           // D-176: the third (the web search's)\n", "", 1)),
    ("a refused tavily.txt keeps the key", "ai_net.cpp",
     lambda t: t.replace("          memset(s_wcfg, 0, sizeof(*s_wcfg));\n          s_wHaveGood = false;\n          s_m->wkey.state = rc == WEB_CFG_NO_KEY",
                         "          s_m->wkey.state = rc == WEB_CFG_NO_KEY", 1)),
    ("an unreadable tavily.txt usable with no good read", "ai_net.cpp",
     lambda t: t.replace("      s_m->wkey.usable = s_wHaveGood;\n      if (s_wHaveGood) {", "      s_m->wkey.usable = true;\n      if (s_wHaveGood) {", 1)),
    ("tavily.txt's buffer freed unwiped", "ai_net.cpp",
     lambda t: _nth(t, "      geminiWipe(buf, AI_KEY_READ_MAX);\n      free(buf);", "      memset(buf, 0, AI_KEY_READ_MAX);\n      free(buf);", 2)),
    ("a tvly- key into gemini.txt", "gemini.cpp",
     lambda t: t.replace("  if (rc == GEM_CFG_OK && !strncmp(out->key, GEM_TAVILY_KEY_PREFIX, sizeof(GEM_TAVILY_KEY_PREFIX) - 1)) {",
                         "  if (false) {", 1)),
    ("tavily.txt takes any key", "websearch.cpp",
     lambda t: t.replace("  } else if (strncmp(g->key, WEB_KEY_PREFIX, sizeof(WEB_KEY_PREFIX) - 1) != 0) {", "  } else if (false) {", 1)),
    ("any key becomes a Tavily Bearer", "websearch.cpp",
     lambda t: t.replace("  if (!key || strncmp(key, WEB_KEY_PREFIX, sizeof(WEB_KEY_PREFIX) - 1) != 0 || !key[sizeof(WEB_KEY_PREFIX) - 1]) {",
                         "  if (!key) {", 1)),
    ("the web block in a log line", "ai_net.cpp",
     lambda t: t.replace("  r->webNote = note;\n  webAppendLine(", "  r->webNote = note;\n  log_e(\"AI: web %s\", s_work->web);\n  webAppendLine(", 1)),
    ("the web block saved as the answer", "ai_net.cpp",
     lambda t: t.replace('    const char* a = cancelled ? "Cancelled" : (ok ? s_res->answer : s_res->fail);',
                         '    const char* a = cancelled ? "Cancelled" : (ok ? s_work->web : s_res->fail);', 1)),
    ("the background folded into the chat", "ai_net.cpp",
     lambda t: t.replace("    geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, a,",
                         "    geminiChatFold(c, s_req->askedUtc, s_req->topic, !ok, s_res->label, s_req->question, s_work->bg,", 1)),
    ("a question's block printed by `ai web`", "ai_net.cpp",
     lambda t: t.replace("  if (!s_req->webOnly) {\n    return;                                        // a question's block is never printed",
                         "  if (false) {\n    return;                                        // a question's block is never printed", 1)),
    ("the web's line over Wikipedia's", "ai_net.cpp",
     lambda t: t.replace("  webAppendLine(s_work->src, sizeof(s_work->src), note == WEB_NOTE_NONE ? s_work->webSrc : webNoteLine(note));",
                         "  strlcpy(s_work->src, note == WEB_NOTE_NONE ? s_work->webSrc : webNoteLine(note), sizeof(s_work->src));", 1)),
    ("the last question's sources kept (aiWikiLookup does not clear them first)", "ai_net.cpp",
     lambda t: t.replace("  s_work->bg[0] = '\\0';\n  s_work->src[0] = '\\0';\n  r->wikiWhy[0] = '\\0';", "  s_work->bg[0] = '\\0';\n  r->wikiWhy[0] = '\\0';", 1)),
    ("\"Ask with web search\" under Off", "app_ai.cpp",
     lambda t: t.replace("  if (webUsable && webMode != WEB_OFF) {", "  if (webUsable) {", 1)),
    ("the Web search row cycles without a key", "app_ai.cpp",
     lambda t: t.replace("        if (!aiWebUsable()) {\n          enterState(AI_KEYINFO);\n        } else {\n          aiSetWebMode(",
                         "        if (false) {\n          enterState(AI_KEYINFO);\n        } else {\n          aiSetWebMode(", 1)),
    ("\"Ask with web search\" outlives its compose", "app_ai.cpp",
     lambda t: t.replace("  if (s != AI_COMPOSE) {\n    askWeb = false;", "  if (false) {\n    askWeb = false;", 1)),
    ("the compose's web flag never reaches aiAsk", "app_ai.cpp",
     lambda t: t.replace("  if (aiAsk(q, noteBuf, sizeof(noteBuf), askWeb)) {", "  if (aiAsk(q, noteBuf, sizeof(noteBuf))) {", 1)),
    ("a stray tavily.txt cloned", CLONE,
     lambda t: t.replace(" --exclude '[Tt][Aa][Vv][Ii][Ll][Yy].[Tt][Xx][Tt]'", "", 1)),
    ("tavily.txt counted on both sides", CLONE,
     lambda t: t.replace(" ! -iname 'tavily.txt'", "", 1)),
    ("tavily.txt no key file", "files_paths.h",
     lambda t: t.replace(' ||\n                                   strncasecmp(p, "tavily.txt", 10) == 0)', ')', 1)),
    # ── the D-176 review (2026-10-19): each gap it found, re-made ──
    ("a search that never reached Tavily counted (a captive portal spends the day's)", "ai_net.cpp",
     lambda t: t.replace("    if (s_res->webSent) {\n      webDayCount(", "    if (s_res->webAsked) {\n      webDayCount(", 1)),
    ("every attempt marked sent", "ai_net.cpp",
     lambda t: t.replace("  r->webSent = res.largestAfterSend != 0 || code > 0;", "  r->webSent = true;", 1)),
    ("a search counted on the attempt again", "ai_net.cpp",
     lambda t: t.replace("    dayLoad();\n    if (s_res->webSent) {\n      webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);\n"
                         "      daySave();\n    }\n",
                         "    dayLoad();\n    webDayCount(&s_m->day, webToday(), s_req->webDec.automatic);\n    daySave();\n", 1)),
    ("\"Ask with web search\" asks without a search", "app_ai.cpp",
     lambda t: t.replace("        askWeb = k == ROW_ASKWEB && next == AI_COMPOSE;", "        askWeb = false;", 1)),
    ("\"Ask with web search\" does nothing", "app_ai.cpp",
     lambda t: t.replace("      if (k == ROW_ASK || k == ROW_ASKWEB) {", "      if (k == ROW_ASK) {", 1)),
    ("the cap's note never shown", "ai_net.cpp",
     lambda t: t.replace("  s_req->webNote = s_req->webDec.why == WEB_WHY_CAP ? WEB_NOTE_CAP : WEB_NOTE_NONE;",
                         "  s_req->webNote = WEB_NOTE_NONE;", 1)),
    ("no \"Searching the web...\" stage", "ai_net.cpp",
     lambda t: t.replace("  progSet(WEB_STAGE, NULL, 0, -1);", "  progSet(NULL, NULL, 0, -1);", 1)),
    ("every failure silent under the answer", "ai_net.cpp",
     lambda t: t.replace("  webAppendLine(s_work->src, sizeof(s_work->src), note == WEB_NOTE_NONE ? s_work->webSrc : webNoteLine(note));",
                         "  webAppendLine(s_work->src, sizeof(s_work->src), s_work->webSrc);", 1)),
    ("no key that can go: no note", "ai_net.cpp",
     lambda t: t.replace("    r->webNote = WEB_NOTE_UNAVAILABLE;             // no body or no key that can go (webParseConfig refused it first)\n"
                         "    webAppendLine(s_work->src, sizeof(s_work->src), webNoteLine(r->webNote));\n",
                         "    r->webNote = WEB_NOTE_UNAVAILABLE;             // no body or no key that can go (webParseConfig refused it first)\n", 1)),
    ("the cap's note not appended", "ai_net.cpp",
     lambda t: t.replace("    if (r->webNote != WEB_NOTE_NONE) {\n      webAppendLine(", "    if (false) {\n      webAppendLine(", 1)),
    ("a Clear chat during `ai web test` drops the next answer", "ai_net.cpp",
     lambda t: t.replace("    s_breakAfter = false;\n    s_clearAfter = false;\n    s_haveRes = true;", "    s_breakAfter = false;\n    s_haveRes = true;", 1)),
    ("a New topic during `ai web test` breaks the next topic", "ai_net.cpp",
     lambda t: t.replace("    s_breakAfter = false;\n    s_clearAfter = false;\n    s_haveRes = true;", "    s_clearAfter = false;\n    s_haveRes = true;", 1)),
    ("`ai web test` keeps the last question's block", "ai_net.cpp",
     lambda t: t.replace("  memset(s_work->web, 0, sizeof(s_work->web));\n  memset(s_work->webSrc, 0, sizeof(s_work->webSrc));\n  memset(s_res, 0,",
                         "  memset(s_work->webSrc, 0, sizeof(s_work->webSrc));\n  memset(s_res, 0,", 1)),
    ("`ai web test` keeps the last question's line", "ai_net.cpp",
     lambda t: t.replace("  memset(s_work->web, 0, sizeof(s_work->web));\n  memset(s_work->webSrc, 0, sizeof(s_work->webSrc));\n  memset(s_res, 0,",
                         "  memset(s_work->web, 0, sizeof(s_work->web));\n  memset(s_res, 0,", 1)),
    ("`ai web` prints a block no search of the test's made", "ai_net.cpp",
     lambda t: t.replace("  if (s_res->webAsked && s_res->webNote == WEB_NOTE_NONE && s_work && s_work->web[0]) {", "  if (s_work && s_work->web[0]) {", 1)),
    ("a question's block outlives its question", "ai_net.cpp",
     lambda t: t.replace("    if (!q->webOnly) {\n", "    if (false) {\n", 1)),
]


def _nth(t, old, new, n):
    """`old` -> `new` at its n-th occurrence (1-based) only; unchanged when there is none."""
    at = -1
    for _ in range(n):
        at = t.find(old, at + 1)
        if at < 0:
            return t
    return t[:at] + new + t[at + len(old):]


def selftest(texts):
    if check(texts):
        return False
    ok = True
    for what, name, f in MUTATIONS:
        mutated = dict(texts)
        mutated[name] = f(texts[name])
        if mutated[name] == texts[name]:
            print("  SELF-TEST: %s - the mutation no longer applies (update it with the code)" % what)
            ok = False
            continue
        if not check(mutated):
            print("  SELF-TEST: %s - not caught" % what)
            ok = False
    return ok


def main():
    texts = load()
    problems = check(texts)
    for p in problems:
        print("  FAIL " + p)
    if problems:
        print("check_ai_glue: %d problem(s)" % len(problems))
        return 1
    if not selftest(texts):
        print("check_ai_glue: SELF-TEST FAILED")
        return 1
    print("check_ai_glue: ok (27 contracts, %d mutations caught)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
