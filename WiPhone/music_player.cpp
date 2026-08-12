/*
 * music_player.cpp — see music_player.h for why this is not inside the app.
 */

#include "music_player.h"
#include "Audio.h"
#include "Hardware.h"
#include "Arduino.h"
#include "SD.h"
#include <string.h>

extern Audio* audio;

// ─── state, with static lifetime so it survives the screen closing ───────────────────

static MusicTrack* s_tracks = NULL;      // PSRAM
static int         s_count = 0;
static MusicQueue  s_queue;
static bool        s_paused = false;     // a track is loaded but not running
static int         s_loaded = -1;        // library index of that track, or -1
static uint32_t    s_startedAt = 0;
static uint32_t    s_elapsedBase = 0;    // seconds already played before the last resume
static const char* s_error = NULL;
static bool        s_began = false;

/* Volume, in dB, RAM-only — see music_player.h. */
static int         s_vol = MUSIC_VOL_DEFAULT_DB;
static bool        s_volSaved = false;      // are the call levels stashed?
static int8_t      s_savedEar = 6, s_savedHp = 6, s_savedLoud = 0;

/* Take the codec over, remembering what calls were using. */
static void applyMusicVolume() {
  if (!audio) {
    return;
  }
  if (!s_volSaved) {
    audio->getVolumes(s_savedEar, s_savedHp, s_savedLoud);
    s_volSaved = true;
  }
  const int8_t v = (int8_t)s_vol;
  // The loudspeaker amp maxes out at 0 dB where the other two reach +6.
  const int8_t loud = v > 0 ? 0 : v;
  audio->setVolumes(v, v, loud);
}

/* Give the codec back at the levels the phone had before music started. Without this a
 * quiet album leaves the next call inaudible. */
static void restoreCallVolume() {
  if (audio && s_volSaved) {
    audio->setVolumes(s_savedEar, s_savedHp, s_savedLoud);
    s_volSaved = false;
  }
}

int musicPlayerVolume() {
  return s_vol;
}

void musicPlayerVolumeStep(int steps) {
  s_vol += steps * MUSIC_VOL_STEP_DB;
  if (s_vol > MUSIC_VOL_MAX_DB) {
    s_vol = MUSIC_VOL_MAX_DB;
  }
  if (s_vol < MUSIC_VOL_MIN_DB) {
    s_vol = MUSIC_VOL_MIN_DB;
  }
  // Only touch the codec while we own it; otherwise this is just a remembered number.
  if (s_volSaved) {
    const int8_t v = (int8_t)s_vol;
    audio->setVolumes(v, v, v > 0 ? 0 : v);
  }
}

void musicPlayerBegin() {
  if (s_began) {
    return;
  }
  s_began = true;
  /* PSRAM: 96 tracks x ~200 bytes is nearly 19 KB, and the internal heap has about 16 KB
   * free in total. This is the same rule that BooksApp learned the hard way. */
  s_tracks = (MusicTrack*)ps_malloc(sizeof(MusicTrack) * MUSIC_MAX_TRACKS);
  if (s_tracks) {
    memset(s_tracks, 0, sizeof(MusicTrack) * MUSIC_MAX_TRACKS);
  }
  s_queue.seed((uint32_t)esp_random());
  musicPlayerScan();
}

// ─── the library ────────────────────────────────────────────────────────────────────

static void addFrom(const char* dirPath) {
  File dir = SD.open(dirPath);
  if (!dir) {
    return;
  }
  if (!dir.isDirectory()) {
    dir.close();
    return;
  }
  File f;
  while (s_count < MUSIC_MAX_TRACKS && (f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      if (musicIsPlayable(nm)) {
        char disp[MUSIC_NAME_MAX];
        musicDisplayName(nm, disp, sizeof(disp));

        bool dup = false;
        for (int i = 0; i < s_count; i++) {
          if (strcmp(s_tracks[i].name, disp) == 0) {
            dup = true;      // the same track in two folders is listed once
          }
        }
        if (!dup) {
          snprintf(s_tracks[s_count].name, MUSIC_NAME_MAX, "%s", disp);
          snprintf(s_tracks[s_count].path, MUSIC_PATH_MAX, "%s", nm);
          s_tracks[s_count].fmt = musicFormatOf(nm);
          s_count++;
        }
      }
    }
    f.close();
  }
  dir.close();
}

int musicPlayerScan() {
  if (!s_tracks) {
    return 0;
  }
  /* Remember what is playing by PATH, not by index: a scan can reorder everything, and
   * an index kept across it would silently point at a different song. */
  char playingPath[MUSIC_PATH_MAX];
  playingPath[0] = '\0';
  if (s_loaded >= 0 && s_loaded < s_count) {
    snprintf(playingPath, sizeof(playingPath), "%s", s_tracks[s_loaded].path);
  }

  /* Four folders, because the uploaders are interchangeable and files genuinely land in
   * the wrong one. `accept=` is a browser hint, not a filter — the Books uploader will
   * happily take an MP3 and put it in /books, which is exactly what happened the first
   * time this app shipped. musicIsPlayable() is what decides, so an .epub sitting in
   * /books never appears here. Books scans /roms for the same reason. */
  s_count = 0;
  addFrom(MUSIC_DIR);
  addFrom("/books");
  addFrom("/roms");
  addFrom("/");
  musicSortTracks(s_tracks, s_count);

  s_queue.reset(s_count);

  s_loaded = -1;
  if (playingPath[0]) {
    for (int i = 0; i < s_count; i++) {
      if (strcmp(s_tracks[i].path, playingPath) == 0) {
        s_loaded = i;
        s_queue.startAt(i);
        break;
      }
    }
    if (s_loaded < 0 && audio) {
      audio->stopMusic();      // the file that was playing is gone
      s_paused = false;
    }
  }
  return s_count;
}

int musicPlayerCount() {
  return s_count;
}

const MusicTrack* musicPlayerTrack(int i) {
  if (!s_tracks || i < 0 || i >= s_count) {
    return NULL;
  }
  return &s_tracks[i];
}

// ─── transport ──────────────────────────────────────────────────────────────────────

/* Stereo follows the headphone jack. Nothing useful comes of driving two channels into
 * a single earpiece, and a track started with headphones in should still be in stereo
 * after the screen has been off. */
static bool wantStereo() {
  return audio && audio->getHeadphones();
}

static bool startTrack(int idx) {
  s_error = NULL;
  if (!audio || !s_tracks || idx < 0 || idx >= s_count) {
    s_error = "No track";
    return false;
  }
  audio->stopMusic();
  if (!audio->playMusic(&SD, s_tracks[idx].path, wantStereo())) {
    s_error = audio->musicError() ? audio->musicError() : "Will not play";
    s_loaded = -1;
    s_paused = false;
    return false;
  }
  applyMusicVolume();
  s_loaded = idx;
  s_paused = false;
  s_startedAt = millis();
  s_elapsedBase = 0;
  return true;
}

bool musicPlayerPlay(int libraryIndex) {
  s_queue.startAt(libraryIndex);
  return startTrack(libraryIndex);
}

void musicPlayerStop() {
  if (audio) {
    audio->stopMusic();
  }
  restoreCallVolume();
  s_loaded = -1;
  s_paused = false;
  s_elapsedBase = 0;
}

void musicPlayerPause() {
  if (audio && audio->musicPlaying()) {
    s_elapsedBase = musicPlayerElapsed();
    audio->stopMusic();
    restoreCallVolume();
    s_paused = s_loaded >= 0;
  }
}

void musicPlayerResume() {
  if (s_paused && s_loaded >= 0) {
    /* Restarts the track from the beginning: there is no seek, because a seek in a VBR
     * MP3 means either an index built by reading the whole file or a guess that lands in
     * the wrong place. Not worth it for a phone you press play on. */
    uint32_t keep = s_elapsedBase;
    if (startTrack(s_loaded)) {
      s_elapsedBase = 0;
      (void)keep;
    }
  }
}

bool musicPlayerTogglePause() {
  if (musicPlayerIsPlaying()) {
    musicPlayerPause();
    return false;
  }
  if (s_paused) {
    musicPlayerResume();
  } else if (s_count > 0) {
    int at = s_queue.current();
    musicPlayerPlay(at >= 0 ? at : 0);
  }
  return musicPlayerIsPlaying();
}

void musicPlayerNext() {
  int n = s_queue.skip(1);
  if (n >= 0) {
    startTrack(n);
  } else {
    musicPlayerStop();
  }
}

void musicPlayerPrev() {
  /* Under three seconds in, Previous means the previous track; after that it means the
   * start of this one. Every music player does this and it is missed when absent. */
  if (musicPlayerElapsed() >= 3 && s_loaded >= 0) {
    startTrack(s_loaded);
    return;
  }
  int p = s_queue.skip(-1);
  if (p >= 0) {
    startTrack(p);
  }
}

bool musicPlayerIsPlaying() {
  return audio && audio->musicPlaying();
}

bool musicPlayerIsPaused() {
  return s_paused;
}

int musicPlayerCurrent() {
  return s_loaded;
}

const char* musicPlayerError() {
  return s_error;
}

uint32_t musicPlayerElapsed() {
  if (!musicPlayerIsPlaying()) {
    return s_elapsedBase;
  }
  return s_elapsedBase + (millis() - s_startedAt) / 1000;
}

void musicPlayerSetShuffle(bool on) {
  s_queue.setShuffle(on);
}
bool musicPlayerShuffle() {
  return s_queue.shuffle();
}
void musicPlayerSetRepeat(MusicRepeat r) {
  s_queue.setRepeat(r);
}
MusicRepeat musicPlayerRepeat() {
  return s_queue.repeat();
}

// ─── the tick ───────────────────────────────────────────────────────────────────────

void musicPlayerLoop() {
  if (!audio || s_paused || s_loaded < 0) {
    return;
  }
  if (!audio->musicPlaying()) {
    return;                      // stopped by something else (a call, an error)
  }
  if (!audio->musicEnded()) {
    return;
  }

  /* End of track. advance() is the end-of-FILE step, so repeat-one replays here — which
   * is deliberately not what the Next key does. See music_lib.h. */
  int nxt = s_queue.advance();
  if (nxt < 0) {
    musicPlayerStop();           // end of the queue with repeat off
    return;
  }
  if (!startTrack(nxt)) {
    /* One bad file should not end the album. Try the one after it, and give up only if
     * the whole queue is unplayable, rather than looping over a broken library. */
    for (int guard = 0; guard < s_count; guard++) {
      int skip = s_queue.advance();
      if (skip < 0 || startTrack(skip)) {
        return;
      }
    }
    musicPlayerStop();
  }
}
