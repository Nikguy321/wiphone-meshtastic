# Changelog

## 0.9.7 — the phone knows where everyone is (no GPS required)

### 📍 Places and positions

COVEY broadcasts its GPS position every few minutes and shares waypoints (camp, the
truck, a stand) from its map. The phone now listens: **Meshtastic → Places** lists every
shared waypoint, and the **Nodes** list shows each node as *"Nick H New Device — 1.4km
NE"* measured from a reference you pick (any waypoint, or your own pin). Positions and
places survive reboots.

**"I'm here"** — open a place and declare yourself at it. The phone remembers (and
shows distances from you), and it announces the position to the mesh once, so COVEY's
map shows the phone. It's a statement, not a fix — the phone has no GPS; a future GPS
module would drop into the same slot (see docs/maps-plan.md for the map-app research).

### 📬 The white "new message" icon tells the truth again

Re-mirrored history (like the chip-erase recovery's) arrived unread and buried itself
deeper than the read-marking scan ever looked — the icon stayed lit with nothing visibly
new. Opening a conversation now clears its unread messages at ANY depth, and the new
serial `unread` command counts the truth from the records themselves, repairs the
counters when they disagree, and names a thread that still holds unread.

### 🔧 For the toolbox

- `pos` — every waypoint, every node fix with age and distance, your pin, the
  reference. The whole picture in one paste.
- `unread` — see above; `unread clear` marks EVERYTHING read, for flags orphaned by a
  deleted conversation (nothing left to open = nothing else could ever clear them).
- Fixed: serial `?` help was silently truncated at 192 bytes — every command added
  since `chan` was invisible in the one place a stranded user looks.

### 🥾 First field test, same day

The whole chain worked on air — waypoint received ("vashon"), reference set, COVEY
placed at "3.2km E of vashon" — but the UI hid the payoff: "Measure from here" now
jumps straight to the Nodes list where the distances live (the first field report was
"nothing happened anywhere?"), and the place-menu labels no longer run off the screen.

### 🛡 From the adversarial review (before this ever shipped)

Positions with modern fields (precision_bits) parse correctly; locked waypoints can't
be moved or deleted by anyone but their owner; a recycled node slot no longer inherits
the evicted node's position or crypto key; restored fixes say "old" instead of a
71,000-minute age; a full node table can't corrupt the waypoint store when RAM is
tight; the "I'm here" announce prefers a PRIVATE channel and says so loudly when only
the public default exists — and if the send fails, Status says "set (send FAILED)"
instead of letting silence read as safety.

## 0.9.6 — direct messages work again: the phone speaks Meshtastic's DM crypto

### 🔐 PKC — the fix for "the RAK refuses every DM to this phone"

Modern Meshtastic (2.5+) requires public-key cryptography for direct messages, in **both
directions**: a stock node refuses to *send* a DM to a phone with no known public key
(`PKI_SEND_FAIL_PUBLIC_KEY`), and silently *drops* the old channel-encrypted DM form on
receive ("Rejecting legacy DM"). Channels never noticed; DMs were dead both ways.

This release implements the whole scheme, verified line-by-line against the 2.7 firmware
source: an X25519 identity keypair (generated once, kept in NVS), the public key riding
in every NodeInfo broadcast, peers' keys learned from theirs (trust-on-first-use, like
stock — a changed key is flagged, never silently swapped), and DMs encrypted with
AES-256-CCM under the SHA-256 of the shared secret. Incoming DMs are ACKed — including
re-ACKing retransmissions — so the sender's app shows ✓ delivered instead of timing out.
DMs to a node whose key hasn't been heard yet still go out the legacy way (a pre-2.5
device will take them) with a loud serial note that a modern node won't.

The crypto is self-contained and host-tested against Python's `cryptography` playing the
stock-firmware side — byte-for-byte frames both directions (`tests/test_pki.cpp`), the
same philosophy as the book-sync hashes. RAM cost: +1.7 KB static, **zero heap**; the
X25519 math runs only at superloop depth where its 3 KB of stack is safe.

### 🔧 For the toolbox

- `pki` — our key (base64, comparable against COVEY's node list), every node's key
  state, mismatch flags, and the loop task's stack headroom.
- `announce` — broadcast NodeInfo now, asking hearers to answer with theirs (the fast
  way to swap keys with the RAK after this update).
- `dm <!node> <text>` — send a DM from the cable; the peer's ACK in the serial log is
  end-to-end proof its firmware decrypted us.
- Fixed while in there: a maximum-length message could build a 256-byte frame whose
  length wrapped to 0 in the radio call (sent nothing, claimed success). Now refused
  with a log line.

## 0.9.5 — install from a browser, browse your files, type just the number

### 🔌 Install and update from a browser

**[nikguy321.github.io/wiphone-meshtastic](https://nikguy321.github.io/wiphone-meshtastic/)** —
plug the phone into a computer, open the link in Chrome or Edge, click **Install**, pick
the port. About a minute; nothing to install on the computer. **Updating never touches
your data** — settings, contacts, messages and the SD card all stay; a full factory-reset
install exists only as a clearly-marked advanced option that asks first. The phone's own
**Settings → Firmware update** screen now leads with this route.

### 🗂 A file browser (Menu → Tools → Files)

Browse the whole SD card. Press OK on a file for **Open / Copy / Move / Delete** —
copy and move use a paste-where-you-land clipboard that survives leaving the app, never
overwrites, and can never damage the original (a move only completes after the copy is
verified). Text files open in a pager that wraps like the e-reader. **Upload into this
folder** turns the WiFi upload page toward wherever you're standing — the whole card is
now reachable over WiFi, not just ROMs and books.

### ☎️ Type just the number

New contact or new text: type `4257604281` and the phone completes it to a full SIP
address using **your own account's server** — so it adapts to any provider with nothing
to configure. Addresses with `@` or letters pass through untouched. Also understands a
leading `+` and entries older versions saved as bare numbers.

### 📖 Reader fix

Some lines lost their first character at the margin (a font measure/render disagreement).
Fixed at the root; wrapping is now exact.

### 🔧 For the toolbox

Serial console additions: `chan <url>` applies a Meshtastic channel invite over the cable
(the mesh can't deliver invites to this phone — modern Meshtastic requires public-key
crypto for DMs that this firmware doesn't implement), `chans` lists channels, `sip`
answers "why is texting dead" in one line, `bookpage` dumps the open reader page.

## 0.9.4 — the audit release: hardening, battery, and small courtesies

### 🛡 Hardening (from a 38-agent audit of the whole codebase)

- **Two remotely-reachable memory-safety holes in the phone stack are closed**: a SIP
  message could overflow a stack buffer via long names/URIs, and a peer sending more than
  100 headers wrote past an array. Both now bounded.
- **A latent double-free in message storage is defused** (partition cache switching
  shallow-copied one cache over another).
- The unread-messages counter no longer under-counts after reading a message when unread
  texts span storage partitions.
- In-call volume keys no longer start from uninitialized memory on a fresh flash, and
  what gets saved is what the hardware actually accepted.
- The mirror poll's stall timeout actually fires now (its re-arm condition was always
  true), and typing-screen redraws survive an out-of-memory strdup.

### 🔋 Battery, for days in the field

- **Out of Wi-Fi range, the radio finally rests.** The phone used to hard-cycle the Wi-Fi
  radio every 20 seconds all day when no network was in range — with the driver
  re-probing in between. After five quick retries it now eases to one attempt every
  3 minutes with the radio quiesced in between; picking the phone up or coming home
  reconnects immediately.
- The LoRa radio's status register is read every 10 ms instead of ~1000 times a second
  (it was bit-banged SPI each pass); nothing can be missed — packets take >100 ms on air.
- When truly idle (screen off, no call, no music, no transfer), the main loop ticks at
  5 ms instead of 1 ms — a fifth of the wakeups, imperceptible latency.

### 📱 Small courtesies

- **Reboot asks first** (it was a one-press menu item next to Settings).
- **Redial**: OK on an empty dialer brings back the last number you called (from the
  dialer or the phonebook); OK again calls it.
- **Battery percentage** next to the icon on the clock screen.
- **# on the clock opens Messages** when the envelope icon is showing.
- **Replying to a text returns you to that conversation**, not the conversations list.
- Serial console gains `bookpage` — dumps the open reader page's layout for diagnosing
  rendering reports.

## 0.9.3 — the day-of-real-use fixes: order, buzz, and the reboots

### 🧾 A conversation reads in the order it happened

- **Texts stored before the phone knew the time no longer pin themselves to the bottom
  forever.** A message arriving between power-on and the first clock sync used to be
  stamped "unknown", which sorted as *newest for all time* — so everything after it
  landed above it, i.e. in the middle of the thread. Now, the moment the clock syncs,
  those messages are stamped with the sync time and fall into place. Their shown time can
  be a little late (the phone has no clock chip to do better); their order is right.
- **Two openings of the same conversation now always show the same order.** Messages
  sharing a timestamp were previously ordered by coin toss. Ties now break on the
  provider's own message number when there is one, and deterministically otherwise —
  which also keeps a burst of catch-up texts from COVEY in their true order.

### 📳 Every arriving text announces itself

- **Texts arriving over WiFi from COVEY buzzed nothing and drew nothing** — they simply
  appeared in the store, and worse, they *claimed* the text so the radio copy stayed
  silent too. Both transports now announce through one shared path: buzz for a text
  somebody sent you, silence for mirrored copies of your own, and an open conversation
  refreshes on screen instead of waiting for you to back out and re-enter.
- The buzz now fires **before** the chirp, so if the chirp's wiring ever interferes with
  the vibration motor again, it loses that race instead of winning it.

### 🧯 The reboots while scrolling or opening conversations

All of the found causes are fixed, and the biggest is structural:

- **Parsed message files now live in the big memory bank (PSRAM), not the small one.**
  Opening a conversation parses message files into hundreds of little objects; they all
  came out of the ~20 KB pool the WiFi/phone stack needs to breathe, and a decoded crash
  backtrace caught exactly that running out. Those objects now go to the 3.6 MB bank.
- **A cached page no longer drags a second message file into memory for nothing**, and a
  long-standing accounting slip (the second file reporting the first file's number) is
  fixed with it.
- **Scrolling a conversation no longer allocates at all** — the redraw made up to 13
  little text copies per keypress, unchecked, and a failed one crashed the phone with the
  trigger "while scrolling". It now draws in place.
- **Running out of room while laying out text truncates the text** instead of writing
  past the end of a block — an out-of-memory moment used to become silent corruption and
  a reboot with no readable crash record.
- **After a reboot, the phone no longer re-downloads and re-checks the entire mirrored
  history from COVEY.** It remembers where it left off on the SD card. (Delete
  `/roms/smsmirror.since` to force a full resync on purpose.)

### 🔇 Housekeeping

- Deleted leftover debug spam that wrote to the field log on every settings keypress.
- Fixed a dormant accessor that returned a message's time when asked for its delivery
  time.

## Unreleased — texting looks like texting, and two devices share one number

### 💬 Messages are conversations now, not an inbox and an outbox

- **One row per person, newest first.** Open it and you see the whole exchange in order,
  oldest at the top, with `You:` on the ones you sent. Replying is one press — it
  already knows who you are replying to.
- The old **Inbox / Sent** split is gone. It was never how anyone thinks about texting;
  it was just how the messages happened to be filed on the card.
- **Unread counts sit on the row**, and opening a conversation clears it.
- The same person is one conversation however their number is written — with the country
  code, without it, punctuated, or as a full address.
- ⚠ The list shows your **recent** conversations, not every message you have ever had.
  Someone you have not texted in a very long time may not appear.

### 📨 Texts sent from another device show up here too

If you run a second device on the same phone number — this was built for COVEY, a
Raspberry Pi handheld — the texts it sends now appear on the phone as well, so the two
do not drift apart.

- **Two ways in, and they cover different gaps.** Over **WiFi** you get the full history,
  including texts that arrived while the phone was off. Over the **radio** (LoRa) you get
  new texts anywhere in range, no WiFi needed. Running both is fine — the phone
  recognises a text it already has and will not show it twice.
- **Setup:** put a two-line `smsmirror.txt` on the SD card — the other device's address
  on the first line, its shared token on the second. Upload it with the same WiFi upload
  page you use for ROMs and books. Nothing happens until that file exists, and the phone
  says so on the serial log rather than sitting there silently.
- ⚠ **Give it its own mesh channel** rather than reusing one. A channel invite contains
  the key, so sharing it hands over everything else on that channel too.
- ⚠ **The WiFi side is unencrypted**, because this phone cannot do HTTPS at all (see
  below). The token stops a stray browser, not somebody watching the same network. On a
  network you do not trust, turn the WiFi side off and let the radio carry it — the radio
  side *is* encrypted.

### 🔌 A serial console, for the things you can otherwise only reach by tapping the screen

Plug in USB, open a terminal at 500000 baud and type `?`. You can start and stop the WiFi
upload page, ask the phone to fetch texts right now, and check what it thinks its state
is — useful when the phone is on a bench and you are not holding it. No password:
whoever has the cable has the phone.

### ℹ️ Why the phone cannot fetch its own texts from the provider

The obvious design would be for the phone to talk to the provider directly. It cannot,
and it is worth writing down so nobody spends a day rediscovering it: a secure connection
wants about 33 KB of a particular kind of memory, and the whole phone has about 31 KB of
it. No setting changes that, and the usual workaround for small devices does not exist in
the version of the encryption library this build is stuck on. It is the same wall that
stops over-the-air firmware updates working.

## Unreleased — it makes phone calls now

### ☎️ Calls and texts work, on a real phone number

- **The phone can call and text ordinary phone numbers, both directions.** A VoIP.ms
  number, over WiFi. Calling out, texting out, ringing on the way in, and texts
  arriving — all confirmed on real hardware against a real mobile.
- ⚠ If inbound calls give a **busy signal** and inbound texts never arrive while
  outbound works fine, the number is routed to the wrong account at the provider,
  not broken on the phone. Point the DID at the sub-account the phone registers as.
- **Put the full address in the phonebook** — `15551234567@yourserver` — not just the
  number. Calling fills in the server for you; texting does not, so a bare number
  calls fine and silently fails to text.

### 🔕 A ringer you can silence

- **Settings > Audio → Ringer:** Ring + vibrate, **Vibrate only**, or Silent. The phone
  always vibrated on an incoming call; now you can keep the buzz and drop the noise.
  Remembered across reboots, and applied from the moment the phone starts rather than
  the first time you visit the settings screen.

### 📬 The status bar says what kind of message is waiting

- **A white envelope means a text; a green one means a Meshtastic message.** Both
  waiting shows them overlapping. Read one and the icon collapses to whichever kind is
  still unread, so it tells you what is left rather than just "something arrived".

### 🧊 The phone stopped freezing for seconds at a time

- **Those five-second freezes while clicking around were not crashes** — the phone was
  stuck waiting on a network name lookup, and everything on screen shares one thread.
  It was asking the local network about internet addresses it could never answer for,
  half a second each time, and retrying a failing lookup twice a second. It now skips
  the pointless question and remembers failures instead of repeating them.

## Unreleased — typing, and honest buttons

### ⌨️ Typing the same letter twice no longer means waiting

- **The middle of the D-pad now means "yes, that letter".** Typing `AZAZ` was
  always quick, because moving to a different key finishes the letter you were
  on. Typing `AAA` was not: pressing `2` again cycles `a → b → c`, so the only
  way to get a second `a` was to stop and wait two seconds. Now you press the
  key, press OK, and it is committed — as fast as your thumbs go, the way old
  phones worked.
- **The two-second wait still works exactly as before.** OK is an extra way to
  finish a letter, not a replacement, so nothing you already do stops working.
- **Sending is unchanged.** The top-left **Send** key and the green call key
  still finish the letter you are on *and* send, in one press. Only the middle
  of the D-pad changed, and only while a letter is waiting.

### 🏷️ The button under "Cancel" never cancelled

- **Four screens labelled a button "Cancel" when it actually deletes a
  character** — writing a message, editing your node name or short name, and the
  book sync settings. The key under that label is backspace; cancelling is the
  key *below* it. They now say **Clear**, which is what the rest of the phone
  already called it.

### 🔌 Firmware updates: honest instructions instead of a broken button

- **Settings > Firmware update** used to offer a Check button that could only
  ever fail. It now shows **step-by-step instructions for updating over USB**,
  on the phone's own screen, so you can read them while holding the phone —
  scroll with up/down, jump a page with left/right.
- **Why the old one could not work:** the phone cannot open a secure connection
  at all. It needs about 33 KB of fast memory to do so and has about 19 KB in
  total. That is a hardware limit, not an expired certificate or a stale link,
  and no amount of retrying was ever going to fix it.
- **The phone also stopped trying at every startup.** It had been attempting that
  same impossible connection on every single boot — quietly, with nothing on
  screen to say so — and the attempt costs both time and the scarce memory the
  phone then goes on to need. Measured on the device afterwards: the largest
  block of free fast memory at startup went from about 14.4 KB to about 15.9 KB,
  which is the number that decides whether the phone stays up.
- The new screen costs the phone **less** memory than the settings form it
  replaced, and does not allocate anything to display its text.

## 0.9.2 — book sync works, stability, and Game Boy speed

⚠ **Known limitation: over-the-air updates do not work on this phone**, and the
reason turns out to be memory rather than anything to do with the link. A secure
connection needs about 33 KB of the phone's scarce fast memory and there is only
about 19 KB of it, so the connection can never be opened. Install over USB. An
earlier note claiming the updater was working again was only half right — the
expired certificate it fixed was a real problem, but not the one stopping it.

### 📖 Reading position sync works between devices

- **Your place in a book now travels over the mesh, for real.** Read on the phone,
  tap *Sync my place*, and the other device offers to jump to where you got to.
  This had been built and tested against recorded data for months but had never
  been tried between two actual devices until now.
- **It works both ways** — phone to COVEY and COVEY to phone.
- **Only the device you are reading on needs the tap.** Tapping *Sync my place*
  sends your position out; the other device picks it up on its own, whatever it
  happens to be doing. You do not have to tap on both.
- ⚠ **Worth knowing, because it looks broken the first time:** the receiving
  device shows nothing at all when the position arrives. Radio messages are slow,
  so it is saved quietly and offered **the next time you open that book**. If sync
  seems to do nothing, open the book on the other device before assuming it failed.
- **Books > menu > Sync settings** tells you what is going on: whether the shared
  channel exists, and how many positions are waiting to be picked up.

## Stability, and Game Boy speed

A day spent on the phone restarting by itself. The cause is now identified and
fixed, a settings screen that had been quietly broken works again, the Game Boy
runs at full speed again, and the tools for reading what the phone was doing no
longer make the problem worse.

### 🔊 Notification sounds stop hijacking the speaker

- **A Meshtastic notification used to permanently change your audio settings.** The
  little "pop" reconfigures the speaker to play itself — mono, loudspeaker, full
  volume, low quality — and then never put any of it back. So one message left the
  phone that way for good: music afterwards played in mono out of the loudspeaker
  even with headphones plugged in, and the Game Boy dropped to half speed, because
  the emulator takes its timing from the speaker.
  Notification sounds now put everything back exactly as they found it. This was
  most likely the real cause of the half-speed games — a message arrives far more
  often than anyone opens the music player.
- ⚠ Still true: a notification stops whatever track is playing. That is a separate
  problem and is not fixed yet.

### 🎮 Game Boy back to full speed

- **Games were running at half speed.** The emulator deliberately lets the audio
  hardware set its pace — it hands over a frame of sound and waits for the
  speaker to be ready for more, which keeps the game locked to real time with no
  drift. But it only ever told the audio hardware *what rate* to play at, never
  whether the sound was stereo, and it quietly relied on whatever the last thing
  to use the speaker had left behind.
  Once the music player started switching between stereo and mono to follow the
  headphone jack, the Game Boy could inherit mono — and then the hardware drained
  its sound at exactly half the expected rate, so the game waited twice as long
  for every frame and ran at half speed. It now sets both, and never inherits.
- ⚠ Note for the future: the on-screen speed readout going orange, and the
  emulator dropping display frames to compensate, both look like "the processor
  is too slow". Neither was. Skipping frames cannot fix a game that is being
  paced by sound.

### 🔋 Battery

- **The phone could get stuck running flat out, on battery, with the screen off.**
  If a call ended while there was no signal, it waited forever for a confirmation
  that could never arrive — and while it waited, it treated itself as being on a
  call and never slowed the processor down. Caught in a car log: nineteen straight
  minutes at full speed, screen off, out of range. That is roughly double the
  idle drain, in exactly the situation where battery matters most.

### 🔁 The random restarts

- **Opening Books was quietly eating the phone's working memory.** Every time you
  opened it, about 3 KB of the phone's small pool of contiguous memory went and
  did not come back — and after enough opens, the next thing that needed a
  contiguous block could not get one and the phone restarted. It now lives in the
  large memory the phone has plenty of. Measured before and after: the cost of
  opening Books fell from about 3,000 bytes to 140.
- ⚠ Note for anyone reading older notes: Books' big *lists* were moved out of the
  scarce memory a while ago, but the app itself was left behind. "Books was fixed"
  was only half true, and the remaining half is what kept causing restarts.

- **The likely cause is fixed.** Every time the phone tried to join WiFi it
  registered another copy of its network event handler, and never removed any of
  them. Out of range it retries every 20 seconds, so a drive with no signal
  stacked up hundreds of copies — one measured journey was 143 minutes without
  service, which is around 430. The moment signal came back, **all** of them ran
  at once, each tearing down and rebuilding the same network buffers. That churn
  breaks the phone's small pool of working memory into fragments until something
  eventually cannot find the unbroken space it needs, and the phone reboots.
  It is registered once now, when the phone starts, instead of on every attempt.

- **The phone can now say why it restarted, and it has been answered.** Four
  restarts were captured and every one of them was a crash — not a flat battery,
  not a stall, not the power being interrupted. The battery never dropped below
  3.99 V across the whole record, which rules that out entirely.

- **First real evidence it worked.** An 85-minute stretch out of signal — the same
  condition that caused a restart in the car — with the phone's free-memory figure
  not moving by a single byte, at nearly three times the headroom the failing run
  had. Still not a complete proof: the phone never rejoined a network in that
  window, which is the other half of the problem.

- **The health log no longer throws away its own evidence.** It kept a fixed
  amount of history and, when full, deleted all of it and started again. That is
  exactly what happened during a restart nobody was watching: the record of *why*
  it restarted was erased half an hour later, before it could be read. It now
  keeps the most recent few hours instead.

### 🧭 Settings

- **Settings > WiFi auto-switch works again.** It was showing the Music icon, and
  choosing it opened the Music player instead of the toggle — so the screen was
  effectively unreachable. The two entries had been given the same internal ID and
  the menu picks the first match, so the WiFi row was standing in for Music.
  Renumbered.

- **The phone now checks its own menu for repeated IDs when it starts.** The same
  mistake had already shipped once before, as "Music opens Books", and was fixed
  in a way that moved the clash rather than removing it. Nothing was watching for
  it, so now something is.

### 🩺 Diagnostics

- **The health log can be read without disturbing what it measures.** Pulling the
  whole file kept truncating, and a truncated read loses the *end* — which is
  exactly where the reason for the last restart is written. Retrying was worse
  than the problem: around thirty fetches, each its own connection, on a phone
  whose scarcest resource is the memory all of that consumes. It can now hand back
  just the most recent part, so one request gets what matters.

## 2026-08-14 — Music player, e-reader, real battery life, and working firmware updates

The biggest release so far. Two whole new apps, a serious pass on power, and the
over-the-air updater brought back from the dead and pointed at this repo.

### 🎵 Music player (new)
- **Plays MP3 and WAV straight from the SD card** — no converting anything.
  Menu > Music. Stereo through the headphone jack, mono to the loudspeaker
  otherwise, and it follows the jack live: unplug mid-song and it carries on out
  of the speaker from the same place.
- **The four side buttons are the transport**, from the top: play/pause, next
  (**hold** for previous), volume up, volume down. Holding "previous" behaves
  like any music player — within 3 seconds it goes back a track, after that it
  restarts the current one.
- **Playback keeps going when you leave the screen.** Read a book or check the
  mesh with music playing; tracks advance on their own.
- Shuffle and repeat (off / all / one), a library scanned from `/music`,
  `/books`, `/roms` and the card root, and its **own WiFi uploader**.
- **Volume starts low** (−18 dB) and is remembered until the phone restarts. It
  is deliberately separate from the call volume, so a quiet album can never
  leave you unable to hear the next phone call.
- Pausing and playing **resumes where you were** rather than restarting.

### 📖 E-reader (new)
- **Reads EPUB and plain text from the card**, with pictures inline. Menu >
  Books.
- **Your place is saved** and survives the phone losing power at any instant.
  Position is stored as (chapter, character offset), not a page number, so it
  still means something after a font-size change — and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do
  (it is 3-component only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently.

### 🔋 Battery life
- **The phone now idles.** The main loop used to spin flat out at 240 MHz
  forever, screen off, doing nothing. It now sleeps between passes.
- **The CPU drops to 80 MHz when nothing is happening** and returns to 240 the
  moment the screen comes on, music plays, a call starts, the emulator runs or
  an upload begins.
- **WiFi modem sleep is on.** The receiver used to stay powered continuously;
  it now parks between beacons. Nothing is missed — buffered traffic still
  arrives, a fraction of a second later.
- **Scanning backs off when there is nothing to join.** Out of range, it used to
  scan every 2 minutes forever; now it eases to 5 after a sustained absence, and
  snaps back the moment anything connects.

### 🛠 Diagnostics
- **The phone records why it restarted.** The reset reason (crash, watchdog,
  brownout, or a normal power-on) is logged at boot, which turns "it rebooted
  again" into a specific answer.
- **A health line every minute** — free memory, the lowest it has ever been,
  largest free block, battery, CPU speed — written to `/health.log` on the card
  so it survives both a reboot and being unplugged.
- **Read it over WiFi** at `http://wiphone.local/log` with an upload screen open.

### 📤 File transfers
- **The upload page works on a phone now.** It was built around drag-and-drop,
  which does not exist on a touch screen; there is a proper "Choose files"
  button, and the file picker no longer greys everything out.
- **It stops after one page load — fixed.** The server kept the single
  connection slot occupied, so nothing loaded again until it was restarted.
- **Opening an uploader no longer knocks the phone off WiFi.** If the radio was
  still associating it would tear the connection down and host its own network
  instead; and on the way out it never reconnected.
- **Uploads are faster and retry themselves** — a weak link no longer means
  starting over by hand.
- **The screen stays awake while a server is running**, so a long transfer does
  not go dark halfway and look like a crash.

### ⬆️ Firmware updates over the air
- **Settings > Firmware settings works again**, and now points at this repo.
- It was not a dead link: the certificate the phone pinned **expired in April
  2021**, so every check failed the TLS handshake regardless of the URL. It now
  trusts a proper root certificate (valid to 2035) instead of a pinned leaf.
- Releases are published from `ota/` in this repo; `tools/publish_ota.sh` builds
  the binary, writes the manifest and bumps the version together.

### Known issues
- **Slight crackle in music playback.** Much improved — the decoder used to be
  rate-limited so it could never catch up after any interruption — but not
  entirely gone. The now-playing screen shows a `gaps:` counter to help pin it
  down.
- **No seeking within a track**, and no track/artist tags: the library lists
  filenames.

## 2026-07-09 — Reliable menu input, forgiving screen-off, working WiFi auto-switch

### Input
- **Menus register one press per press.** Buttons no longer double-fire, and
  holding the d-pad no longer auto-scrolls the menu — UI key events are now
  strictly edge-triggered (one event per physical press, re-armed only by a
  real release). The Game Boy emulator's input path is untouched.
- **Triple-tap-to-sleep is easy now** — three taps of the top-right button
  with up to ½ second between them (was a near-impossible fixed 0.7 s window
  for all three).

### WiFi
- **Auto-switch actually works.** It reliably scans and connects to the
  strongest saved network now (previously it could sit disconnected
  indefinitely). It also scans more often while disconnected (every 2 minutes)
  so it finds a network without waiting.

## 2026-07-07 — Game Boy polish, big-ROM support, WiFi auto-switch, input reliability

### Game Boy emulator
- **Sound!** The emulated Game Boy APU plays through the phone's speaker
  (32 kHz stereo over I2S). The **top two side keys** adjust volume in-game.
  Audio can never slow the game down: if the audio path misbehaves, the game
  drops sound and keeps running at full speed.
- **Full speed on heavy games** — even demanding Game Boy Color titles
  (e.g. Resident Evil Gaiden: 44% → ~100%). How: removed a PSRAM silicon
  workaround the phone's rev-3 chip doesn't need, hot emulator paths moved to
  zero-wait IRAM, `-O2/-O3` on the emulator core, cache-friendly screen
  blitting, and adaptive frameskip (heavy scenes drop *display* frames, never
  game speed).
- **Big ROMs (4 MB carts) now playable** — ROMs too large to hold in RAM are
  streamed from the SD card in 16 KB banks on demand.
- **Save states fixed** — saving used to reboot the phone (a task stack was
  4× smaller than intended).
- **Transfer ROMs & Help moved into the Game Boy app** — top two rows of the
  game list. The help screen documents controls and everything else.
- **Multi-file ROM upload fixed** — dropping several ROMs at once used to
  reboot the phone (RAM starvation + watchdog); uploads now go one-per-request
  with per-file progress, and the transfer screen frees the emulator's memory
  and the unused Bluetooth reserve before serving.
- **Reliable buttons** — held directions no longer stutter or stick: fixed a
  keypad-handler bug that wiped held keys, added I2C error retry, periodic
  FIFO draining, and a 40 ms hardware "still pressed" heartbeat so a lost
  release un-sticks in a third of a second.
- Long ROM names marquee-scroll in the picker; honest error screens (a ROM
  that can't load no longer silently launches the built-in game).

### Phone
- **WiFi auto-switch** — the phone periodically scans (battery-friendly, async)
  and hops to the strongest *saved* network; if the screen wakes with no
  connection it scans immediately. Only switches when meaningfully stronger
  (10 dB hysteresis). Toggle under **Settings → WiFi auto-switch**.
- **Status bar shows the connected network name** (small text, auto-shortened).
- **"Edit current network" no longer freezes** — a toggle handler ran on every
  keystroke and blocked 5 s reconnecting each time.
- Pressing End during a game no longer triggers the phone's call/hang-up UI.

## 2026-07-05 — Meshtastic: channels, relaying, unread counts

- Multiple channels with AES-128/256; import channels from a Meshtastic share
  link fully offline (apply from a received DM).
- Mesh client role: relays other nodes' packets (flood routing) to extend range.
- Configurable hop limit; per-chat unread counts; status-bar unread icon.
- Game Boy Color emulator first milestones: playable → full-speed dual-core,
  ROM picker, WiFi ROM transfer, save states, pause menu, 1:1/Fill scaling.

## 2026-07-04 — Initial public release

- PlatformIO build of the stock WiPhone firmware (Arduino-IDE only upstream).
- Meshtastic on the WiPhone's SX1276 LoRa radio: two-way encrypted text with
  standard Meshtastic nodes on LongFast (US), chats/threads UI, direct
  messages, node names, persistent history, notifications (icon, popup, sound,
  vibration), editable node name, low-power green/black theme.
