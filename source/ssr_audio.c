/* ssr_audio.c -- the game's two sound outputs, mixed and played through audout.
 *
 * SOUND EFFECTS AND VOICES. libssasr.so has OpenAL Soft 1.13 built in, with
 * its Android backend: a thread of its own mixes the game's sources and
 * writes the PCM to a Java android.media.AudioTrack (constructed through JNI
 * with (streamType, sampleRate, channelConfig, audioFormat, bufferSize,
 * mode), then play() and a loop of write(byte[], 0, n)). On a phone that
 * write blocks until the device has room, which is what paces the mixer.
 * Here the AudioTrack is a C class (ssr_at_* here, in ssr_java.c's table): write()
 * converts the block to 48 kHz stereo s16 (linear resampling, continuous
 * across writes) into a ring buffer and blocks while the ring is full.
 *
 * MUSIC. The Java side plays it (DemoActivity.a / r / s / t: a MediaPlayer
 * on res/raw/music_*.mp3, one track at a time, looped or not, its volume set
 * by the engine). Here a music thread decodes the MP3 straight out of the
 * APK (FFmpeg, ssr_media.c), resamples it to 48 kHz and fills a second ring.
 *
 * The mixer thread (the runtime's pump, rt_audout.c) adds the two rings (and
 * the intro movie's sound, while it plays) into 1024-frame buffers of 48 kHz
 * stereo and keeps three queued on audout: its blocking is the clock of
 * both. HOME holds everything where it is (the writers block on their full
 * rings); closing makes writes return at once, since the engine's shutdown
 * may wait for its sound thread. MIT.
 */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dcr_config.h"
#include "jni.h"
#include "rt_audout.h"
#include "ssr.h"
#include "util.h"

/* ================================================================ audout */
/* The output is the runtime's (rt_audout.c): three buffers of
 * RT_AUDOUT_FRAMES (1024) frames of 48 kHz stereo s16, its blocking submit
 * the clock of both rings; its pump thread runs mix() below. */
#define FRAMES_PER_BUF RT_AUDOUT_FRAMES

static volatile int g_stop_thread, g_paused, g_closing;

/* the watchdog's "is the sound alive": buffers mixed */
unsigned long ssr_audio_mixes(void) { return rt_audout_pump_blocks(); }
uint32_t ssr_audio_writes(void) { return (uint32_t)rt_audout_pump_blocks(); }

/* ================================================================ rings */
/* 48 kHz stereo s16 frames: one writer (it blocks while full), one reader
 * (the mixer: what is not there reads as silence). */
typedef struct {
  Mutex lock;
  CondVar space;
  int16_t *buf;
  int cap, rd, n; /* frames */
} Ring;

static void ring_init(Ring *r, int cap) {
  mutexInit(&r->lock);
  condvarInit(&r->space);
  r->buf = calloc((size_t)cap * 2, sizeof(int16_t));
  r->cap = r->buf ? cap : 0;
  r->rd = r->n = 0;
}

/* Blocks until every frame is in, or *cancel (then the rest is dropped). */
static void ring_write(Ring *r, const int16_t *src, int frames, volatile int *cancel) {
  mutexLock(&r->lock);
  while (frames > 0 && r->cap) {
    while (r->n == r->cap && !*cancel && !g_closing)
      condvarWaitTimeout(&r->space, &r->lock, 20000000ll);
    if (*cancel || g_closing)
      break;
    int wr = (r->rd + r->n) % r->cap;
    int room = r->cap - r->n, chunk = frames < room ? frames : room;
    if (chunk > r->cap - wr)
      chunk = r->cap - wr;
    memcpy(r->buf + wr * 2, src, (size_t)chunk * 4);
    r->n += chunk;
    src += chunk * 2;
    frames -= chunk;
  }
  mutexUnlock(&r->lock);
}

/* Adds up to `frames` frames times gain (1 = 256) to acc; returns how many
 * were there. */
static int ring_mix(Ring *r, int32_t *acc, int frames, int gain) {
  mutexLock(&r->lock);
  int got = frames < r->n ? frames : r->n;
  for (int f = 0; f < got; f++) {
    const int16_t *s = r->buf + ((r->rd + f) % r->cap) * 2;
    acc[f * 2] += (s[0] * gain) >> 8;
    acc[f * 2 + 1] += (s[1] * gain) >> 8;
  }
  r->rd = (r->rd + got) % (r->cap ? r->cap : 1);
  r->n -= got;
  condvarWakeAll(&r->space);
  mutexUnlock(&r->lock);
  return got;
}

static void ring_clear(Ring *r) {
  mutexLock(&r->lock);
  r->rd = r->n = 0;
  condvarWakeAll(&r->space);
  mutexUnlock(&r->lock);
}

static int ring_count(Ring *r) {
  mutexLock(&r->lock);
  int n = r->n;
  mutexUnlock(&r->lock);
  return n;
}

/* A resampler from any rate to the output's: linear, continuous across the
 * blocks it is fed (index -1 is the previous block's last frame). */
typedef struct {
  double pos;
  int16_t prev[2];
} Resampler;

/* in: `frames` interleaved frames of `ch` channels (s16), at `rate`;
 * out: stereo at the device's rate (rt_audout_rate), appended to *out_n (cap in frames). */
static void resample(Resampler *rs, const int16_t *in, int frames, int ch, int rate, int16_t *out, int *out_n,
                     int cap) {
  const double step = (double)rate / (double)rt_audout_rate();
  while (rs->pos < (double)(frames - 1) && *out_n < cap) {
    int i0 = (int)floor(rs->pos);
    double t = rs->pos - (double)i0;
    int16_t a[2], b[2];
    if (i0 < 0) {
      a[0] = rs->prev[0], a[1] = rs->prev[1];
    } else {
      a[0] = in[i0 * ch], a[1] = in[i0 * ch + (ch > 1)];
    }
    b[0] = in[(i0 + 1) * ch], b[1] = in[(i0 + 1) * ch + (ch > 1)];
    out[*out_n * 2] = (int16_t)((double)a[0] + (double)(b[0] - a[0]) * t);
    out[*out_n * 2 + 1] = (int16_t)((double)a[1] + (double)(b[1] - a[1]) * t);
    (*out_n)++;
    rs->pos += step;
  }
  rs->pos -= (double)frames;
  if (frames > 0) {
    rs->prev[0] = in[(frames - 1) * ch];
    rs->prev[1] = in[(frames - 1) * ch + (ch > 1)];
  }
}

static Ring g_music_ring;

/* ========================================================== AudioTrack */
/* android.media.AudioTrack constants */
#define AT_CHANNEL_CONFIGURATION_MONO 2
#define AT_CHANNEL_CONFIGURATION_STEREO 3
#define AT_CHANNEL_OUT_MONO 4
#define AT_CHANNEL_OUT_STEREO 12
#define AT_ENCODING_PCM_16BIT 2
#define AT_ENCODING_PCM_8BIT 3
#define AT_PLAYSTATE_STOPPED 1
#define AT_PLAYSTATE_PAUSED 2
#define AT_PLAYSTATE_PLAYING 3

/* Each AudioTrack has its own ring, mixed with the others': one per OpenAL
 * device, so two copies of the engine (split screen) are two tracks, each
 * written by its own OpenAL thread (its conversion buffers its own too). */
typedef struct {
  int rate, ch, bits;
  int state; /* AT_PLAYSTATE_* */
  float vol;
  float gain; /* the port's, for this track (ssr_audio_track_gain) */
  int engine; /* the engine copy that made it */
  Resampler rs;
  Ring ring;
  int16_t *conv, *out;
  volatile int flush; /* stop()/flush(): a blocked write gives up */
  unsigned long writes;
} Track;

#define MAX_TRACKS 4
static Track *g_tracks[MAX_TRACKS];
static float g_engine_gain[2] = {1.0f, 1.0f}; /* each copy's, for its tracks made later too */
static Mutex g_tracks_lock;

int ssr_engine_current(void); /* which engine copy runs on this thread (0, 1) */

#define H(fn) jvalue ssr_##fn(JObj *self, const jvalue *a, const JMethod *m)

static int channels_of(int cfg) {
  return cfg == AT_CHANNEL_CONFIGURATION_MONO || cfg == AT_CHANNEL_OUT_MONO ? 1 : 2;
}

H(at_min_buffer) {
  /* (sampleRate, channelConfig, audioFormat): ~40 ms, whole 256-frame blocks.
   * OpenAL Soft mixes this many bytes per write, so it is also its latency. */
  int rate = a[0].i > 0 ? a[0].i : 44100, ch = channels_of(a[1].i), bytes = a[2].i == AT_ENCODING_PCM_8BIT ? 1 : 2;
  int frames = ((rate / 25 + 255) / 256) * 256; /* OpenAL asks for 22050 Hz stereo s16: 1024 frames, 46 ms */
  debugPrintf("[audio] AudioTrack.getMinBufferSize(%d Hz, %d ch, %d bit) -> %d\n", rate, ch, bytes * 8,
              frames * ch * bytes);
  return jv_i(frames * ch * bytes);
}

static void track_finalize(JObj *self) {
  Track *t = self->p;
  if (t) {
    mutexLock(&g_tracks_lock);
    for (int i = 0; i < MAX_TRACKS; i++)
      if (g_tracks[i] == t)
        g_tracks[i] = NULL;
    mutexUnlock(&g_tracks_lock);
    free(t->ring.buf);
    free(t->conv);
    free(t->out);
    free(t);
  }
  self->p = NULL;
}

H(at_init) {
  /* (streamType, sampleRate, channelConfig, audioFormat, bufferSizeInBytes, mode) */
  Track *t = calloc(1, sizeof *t);
  if (!t)
    return jv_none();
  t->rate = a[1].i > 0 ? a[1].i : 44100;
  t->ch = channels_of(a[2].i);
  t->bits = a[3].i == AT_ENCODING_PCM_8BIT ? 8 : 16;
  t->state = AT_PLAYSTATE_STOPPED;
  t->vol = 1.0f;
  t->engine = ssr_engine_current();
  t->gain = g_engine_gain[t->engine & 1];
  ring_init(&t->ring, 4096); /* ~85 ms (OpenAL mixes ~40 ms per write) */
  t->conv = malloc(4096 * 2 * sizeof(int16_t));
  t->out = malloc(4096 * 2 * 3 * sizeof(int16_t));
  if (!t->ring.buf || !t->conv || !t->out) {
    free(t->ring.buf), free(t->conv), free(t->out), free(t);
    return jv_none();
  }
  mutexLock(&g_tracks_lock);
  int slot = -1;
  for (int i = 0; i < MAX_TRACKS && slot < 0; i++)
    if (!g_tracks[i])
      slot = i;
  if (slot >= 0)
    g_tracks[slot] = t;
  mutexUnlock(&g_tracks_lock);
  if (slot < 0)
    debugPrintf("[audio] more than %d AudioTracks: this one is not heard\n", MAX_TRACKS);
  self->p = t;
  self->finalize = track_finalize;
  debugPrintf("[audio] new AudioTrack(stream %d, %d Hz, %d ch, %d bit, buffer %d, mode %d)\n", a[0].i, t->rate,
              t->ch, t->bits, a[4].i, a[5].i);
  return jv_none();
}

H(at_play) {
  Track *t = self->p;
  if (t) {
    t->flush = 0;
    t->state = AT_PLAYSTATE_PLAYING;
    debugPrintf("[audio] AudioTrack.play()\n");
  }
  return jv_none();
}

H(at_stop) {
  Track *t = self->p;
  if (t) {
    t->state = AT_PLAYSTATE_STOPPED;
    t->flush = 1;
    ring_clear(&t->ring);
    debugPrintf("[audio] AudioTrack.stop() after %lu writes\n", t->writes);
  }
  return jv_none();
}

H(at_pause) {
  Track *t = self->p;
  if (t)
    t->state = AT_PLAYSTATE_PAUSED;
  return jv_none();
}

H(at_flush) {
  Track *t = self->p;
  if (t)
    ring_clear(&t->ring);
  return jv_none();
}

H(at_release) {
  Track *t = self->p;
  if (t) {
    t->flush = 1;
    t->state = AT_PLAYSTATE_STOPPED;
  }
  return jv_none();
}

H(at_play_state) {
  Track *t = self->p;
  return jv_i(t ? t->state : AT_PLAYSTATE_STOPPED);
}
H(at_state) { return jv_i(self->p ? 1 : 0); } /* STATE_INITIALIZED / UNINITIALIZED */
H(at_rate) {
  Track *t = self->p;
  return jv_i(t ? t->rate : 0);
}
H(at_stereo_volume) {
  Track *t = self->p;
  if (t)
    t->vol = a[0].f > a[1].f ? a[0].f : a[1].f;
  return jv_i(0);
}
H(at_zero) { return jv_i(0); }

/* write(byte[] audioData, int offsetInBytes, int sizeInBytes) -> bytes written */
H(at_write) {
  Track *t = self->p;
  JObj *arr = a[0].l;
  int off = a[1].i, len = a[2].i;
  if (!t || !arr || arr->kind != JK_ARRAY || off < 0 || len <= 0 || off + len > arr->a.len)
    return jv_i(len > 0 ? len : 0);
  while (g_paused && !g_closing) /* in the background: a paused device holds the writer */
    svcSleepThread(10000000ll);
  if (g_closing) {
    svcSleepThread(5000000ll); /* closing: the thread is about to be joined */
    return jv_i(len);
  }
  /* Whatever the track's state, the block goes into the ring: OpenAL's
   * mixer loop has no sleep of its own and keeps writing (silence) even
   * while it has the track paused (nothing playing), so this write is its
   * only pacing (NATIVE_CONTRACT.md 11). */
  if (!t->writes++)
    debugPrintf("[audio] first AudioTrack.write: %d bytes\n", len);
  const int frame_bytes = t->ch * (t->bits / 8);
  const int frames = len / frame_bytes;
  const uint8_t *in = (const uint8_t *)arr->a.data + off;
  /* to s16, then to 48 kHz stereo, in blocks */
  int16_t *conv = t->conv, *out = t->out;
  for (int done = 0; done < frames;) {
    int n = frames - done > 4096 ? 4096 : frames - done;
    if (t->bits == 16) {
      memcpy(conv, in + (size_t)done * frame_bytes, (size_t)n * frame_bytes);
    } else {
      for (int i = 0; i < n * t->ch; i++)
        conv[i] = (int16_t)(((int)in[(size_t)done * frame_bytes + i] - 128) << 8);
    }
    int on = 0;
    resample(&t->rs, conv, n, t->ch, t->rate, out, &on, 4096 * 3);
    if (t->vol < 0.999f)
      for (int i = 0; i < on * 2; i++)
        out[i] = (int16_t)((float)out[i] * t->vol);
    ring_write(&t->ring, out, on, &t->flush);
    done += n;
  }
  return jv_i(len);
}


/* ================================================================ music */
/* DemoActivity.Q: the engine's music index -> res/raw/<name>.mp3 (99 entries,
 * the order of the Java array). */
static const char *const k_music[] = {
    "music_main_menu", "music_mission_menu", "music_network_menu", "music_shop_menu", "music_sm_intro", /* 0..4 */
    "music_sm_main_a", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 5..9 */
    "music_sm_ending", "music_sh_intro", "music_sh_main_a", "music_main_menu", "music_main_menu", /* 10..14 */
    "music_main_menu", "music_main_menu", "music_sh_ending", "music_cp_intro", "music_cp_main_a", /* 15..19 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_cp_ending", /* 20..24 */
    "music_bh_intro", "music_bh_main_a", "music_main_menu", "music_main_menu", "music_main_menu", /* 25..29 */
    "music_main_menu", "music_bh_ending", "music_sa_intro", "music_main_menu", "music_main_menu", /* 30..34 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_sa_main_alt", "music_sa_ending", /* 35..39 */
    "music_ff_intro", "music_ff_main_a", "music_main_menu", "music_main_menu", "music_main_menu", /* 40..44 */
    "music_main_menu", "music_ff_ending", "music_hd_intro", "music_hd_main_a", "music_main_menu", /* 45..49 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_hd_ending", "music_js_intro", /* 50..54 */
    "music_js_main_a", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 55..59 */
    "music_js_ending", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 60..64 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 65..69 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 70..74 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 75..79 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 80..84 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", "music_main_menu", /* 85..89 */
    "music_main_menu", "music_main_menu", "music_main_menu", "music_jingle_rankaaa", "music_jingle_rankaa", /* 90..94 */
    "music_jingle_ranka", "music_jingle_ranke_b", "music_jingle_rankfail", "music_play_mode", /* 95..98 */
};
#define N_MUSIC ((int)(sizeof k_music / sizeof k_music[0]))
_Static_assert(sizeof k_music / sizeof k_music[0] == 99, "DemoActivity.Q has 99 entries");

enum { M_NONE, M_PLAY, M_STOP };
static struct {
  Mutex lock;
  CondVar cv;
  int cmd, cmd_index, cmd_loop; /* the next request (the last one wins) */
  float cmd_vol;
  int index, loop, playing, paused; /* what plays (index -1: nothing) */
  volatile float vol;               /* MediaPlayer.setVolume, 0..1 */
  volatile int abort;               /* the decoder is to drop what it holds */
} g_mu = {.index = -1, .vol = 1.0f};

void ssr_music_play(int index, float volume, int loop) {
  if (index < 0 || index >= N_MUSIC) {
    debugPrintf("[music] javaPlayMusic(%d): no such track\n", index);
    return;
  }
  mutexLock(&g_mu.lock);
  /* DemoActivity.a: the same track while it plays only changes its volume
   * and looping; another one replaces it */
  if (index == g_mu.index && g_mu.playing && !g_mu.paused && g_mu.cmd != M_STOP) {
    g_mu.vol = volume;
    g_mu.loop = loop;
    mutexUnlock(&g_mu.lock);
    return;
  }
  g_mu.cmd = M_PLAY;
  g_mu.cmd_index = index;
  g_mu.cmd_vol = volume;
  g_mu.cmd_loop = loop;
  g_mu.abort = 1;
  condvarWakeAll(&g_mu.cv);
  mutexUnlock(&g_mu.lock);
}

void ssr_music_stop(void) {
  mutexLock(&g_mu.lock);
  g_mu.cmd = M_STOP;
  g_mu.abort = 1;
  condvarWakeAll(&g_mu.cv);
  mutexUnlock(&g_mu.lock);
}

void ssr_music_pause(void) {
  mutexLock(&g_mu.lock);
  g_mu.paused = 1;
  mutexUnlock(&g_mu.lock);
}

void ssr_music_unpause(void) {
  mutexLock(&g_mu.lock);
  g_mu.paused = 0;
  condvarWakeAll(&g_mu.cv);
  mutexUnlock(&g_mu.lock);
}

void ssr_music_volume(float v) { g_mu.vol = v < 0 ? 0 : v > 1 ? 1 : v; }

static void music_thread(void *arg) {
  (void)arg;
  static int16_t dec[4608 * 2];
  static int16_t out[4608 * 2 * 3];
  SsrMp3 *mp3 = NULL;
  Resampler rs = {0};
  for (;;) {
    mutexLock(&g_mu.lock);
    while (g_mu.cmd == M_NONE && (!g_mu.playing || g_mu.paused) && !g_stop_thread)
      condvarWaitTimeout(&g_mu.cv, &g_mu.lock, 100000000ll);
    int cmd = g_mu.cmd, idx = g_mu.cmd_index, loop = g_mu.cmd_loop;
    float vol = g_mu.cmd_vol;
    g_mu.cmd = M_NONE;
    g_mu.abort = 0;
    if (cmd == M_STOP) {
      g_mu.playing = 0;
      g_mu.index = -1;
    } else if (cmd == M_PLAY) {
      g_mu.index = idx, g_mu.loop = loop, g_mu.vol = vol, g_mu.playing = 1, g_mu.paused = 0;
    }
    mutexUnlock(&g_mu.lock);
    if (g_stop_thread)
      break;
    if (cmd != M_NONE) {
      ring_clear(&g_music_ring);
      if (mp3)
        ssr_mp3_close(mp3);
      mp3 = NULL;
      memset(&rs, 0, sizeof rs);
    }
    if (cmd == M_PLAY) {
      char name[64];
      snprintf(name, sizeof name, "res/raw/%s.mp3", k_music[idx]);
      uint64_t off = 0;
      uint32_t len = 0;
      int method = -1;
      if (ssr_apk_locate(name, &off, &len, &method) != 0 || method != 0)
        debugPrintf("[music] %s: not in the APK as a stored entry\n", name);
      else
        mp3 = ssr_mp3_open(ssr_apk_path(), off, len);
      debugPrintf("[music] %d: %s (%s, volume %.2f)%s\n", idx, k_music[idx], loop ? "looped" : "once", (double)vol,
                  mp3 ? "" : " -- could not open it");
      if (!mp3) {
        mutexLock(&g_mu.lock);
        g_mu.playing = 0;
        mutexUnlock(&g_mu.lock);
      }
    }
    if (!mp3 || !g_mu.playing || g_mu.paused)
      continue;
    int n = ssr_mp3_read(mp3, dec, 4608);
    if (n <= 0) {
      if (g_mu.loop) {
        ssr_mp3_rewind(mp3);
        continue;
      }
      ssr_mp3_close(mp3);
      mp3 = NULL;
      mutexLock(&g_mu.lock);
      g_mu.playing = 0;
      mutexUnlock(&g_mu.lock);
      continue;
    }
    int on = 0;
    resample(&rs, dec, n, 2, ssr_mp3_rate(mp3), out, &on, 4608 * 3);
    ring_write(&g_music_ring, out, on, &g_mu.abort);
  }
  if (mp3)
    ssr_mp3_close(mp3);
}

/* ============================================================ the mixer */
static Thread g_music_thread;

/* the pump's fill (rt_audout.c): both rings and the movie's sound, one buffer */
static void mix(int16_t *out, int frames, void *ud) {
  (void)ud;
  static int32_t acc[FRAMES_PER_BUF * 2];
  memset(acc, 0, sizeof acc);
  const DcrConfig *c = dcr_config();
  int mu_gain = g_mu.paused ? 0 : (int)(256.0f * c->music_volume * g_mu.vol);
  int fx_queued = 0;
  mutexLock(&g_tracks_lock);
  for (int i = 0; i < MAX_TRACKS; i++)
    if (g_tracks[i]) {
      ring_mix(&g_tracks[i]->ring, acc, frames, (int)(256.0f * c->sfx_volume * g_tracks[i]->gain));
      fx_queued += ring_count(&g_tracks[i]->ring);
    }
  mutexUnlock(&g_tracks_lock);
  if (mu_gain)
    ring_mix(&g_music_ring, acc, frames, mu_gain);
  for (int i = 0; i < frames * 2; i++) {
    int32_t x = acc[i];
    out[i] = (int16_t)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
  }
  ssr_video_mix(out, frames, (int)rt_audout_rate());
  const unsigned long mixes = rt_audout_pump_blocks() + 1; /* this one */
  if (mixes % 3000 == 0) {
    RtAudoutStats st;
    rt_audout_stats(&st);
    debugPrintf("[audio] %lu mixes; %lu underruns, %lu failed submits (%lu dropped); queued fx %d, music %d\n",
                mixes, st.underruns, st.append_fails, st.dropped, fx_queued, ring_count(&g_music_ring));
  }
}

void ssr_audio_init(void) {
  mutexInit(&g_mu.lock);
  condvarInit(&g_mu.cv);
  /* each track's ring is its own (at_init); the music's queues more */
  mutexInit(&g_tracks_lock);
  ring_init(&g_music_ring, 8192);
  /* priority 0x28: above the game's threads (59 in libnx terms is lower), so
   * the mixer keeps up; the music decoder just below it */
  if (rt_audout_pump_start(mix, NULL, 0x28, 2) != 0)
    return;
  if (R_FAILED(threadCreate(&g_music_thread, music_thread, NULL, NULL, 0x20000, 0x2A, 2)) ||
      R_FAILED(threadStart(&g_music_thread)))
    debugPrintf("[audio] could not start the music thread\n");
}

void ssr_audio_pause(int paused) {
  g_paused = paused; /* the writers hold */
  rt_audout_pause(paused); /* the mixer holds */
}

void ssr_audio_close(void) {
  g_closing = 1;
  ssr_music_stop();
}

/* The port's gain for one engine copy's sound (split screen: the second
 * one's effects a little lower, as they double the first's). */
void ssr_audio_engine_gain(int engine, float gain) {
  mutexLock(&g_tracks_lock);
  g_engine_gain[engine & 1] = gain;
  for (int i = 0; i < MAX_TRACKS; i++)
    if (g_tracks[i] && g_tracks[i]->engine == engine)
      g_tracks[i]->gain = gain;
  mutexUnlock(&g_tracks_lock);
}
