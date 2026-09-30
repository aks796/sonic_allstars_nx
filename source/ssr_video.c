/* ssr_video.c -- the intro movie (intro_full.m4v).
 *
 * On a phone DemoRenderer's splash sequence ends with DemoActivity.u(): a
 * VideoView over the game plays content://.../ZipFileContentProvider/
 * intro_full.m4v -- a stored entry of the expansion file, which APEZProvider
 * hands MediaPlayer as a byte range -- while the engine keeps running (and
 * loading) underneath; a touch or the end hides it. Here the same bytes are
 * read in place from wherever the pack is (ssr_pack.h: the .obb, the download
 * zip, or the single APK) through a custom AVIOContext; FFmpeg (portlibs32/:
 * the MOV demuxer, H.264 and AAC decoders, LGPL) decodes it on a thread of its
 * own; each picture is converted to RGBA (NEON) and drawn over the game's
 * picture before each present (ssr_video_draw, gl_blit.c keeping the game's
 * GLES 1 state); the sound is mixed into the game's output (ssr_video_mix).
 * The pictures follow the clock started at the first one shown. Any button
 * or a touch ends it (ssr_video_skip), as a tap did on the phone.
 *
 * From the PvZ port's pvz_video.c. This file is compiled with
 * -fno-short-enums, as FFmpeg is. MIT.
 */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <arm_neon.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "gl_blit.h"
#include "gl_layer.h"
#include "ssr.h"
#include "util.h"

#if !DCR_VIDEO /* built without FFmpeg: no intro */
int ssr_video_start(void) {
  debugPrintf("[video] this build has no video decoder: no intro\n");
  return 0;
}
int ssr_video_playing(void) { return 0; }
void ssr_video_skip(void) {}
void ssr_video_draw(void) {}
void ssr_video_mix(int16_t *out, int frames, int out_rate) { (void)out, (void)frames, (void)out_rate; }
#else

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

void dcr_window_size(int *w, int *h);
void dcr_boost_hold(int on); /* dcr_boost.c */

/* FFmpeg 7.1's h2645_sei.c resets the AOM film grain sets unconditionally,
 * but aom_film_grain.o is only built with HEVC, which this build (ffmpeg32
 * with the components in portlibs32/README.md) leaves out; without HEVC
 * nothing ever fills them (their parser sits behind IS_HEVC(), constant
 * false), so there is nothing to reset. As abs_video.c does. */
void ff_aom_uninit_film_grain_params(void *s);
void ff_aom_uninit_film_grain_params(void *s) { (void)s; }

#define NSLOT 4 /* pictures decoded ahead */

static struct {
  Mutex lock;
  int open, playing, finished;
  volatile int stop, skip, eof;
  FILE *f;
  uint64_t base;
  int64_t len, pos;
  AVFormatContext *fmt;
  AVIOContext *io;
  AVCodecContext *vdec, *adec;
  int vs, as;
  double vtb; /* the video stream's time base, in seconds */
  Thread thread;
  int thread_on;
  int w, h;
  uint8_t *rgba[NSLOT];
  double pts[NSLOT];
  volatile int ready[NSLOT];
  int shown; /* the slot on the texture; -1 none */
  double shown_pts;
  int started; /* the clock runs */
  u64 t0;
  /* sound: all of it, s16 stereo at arate */
  int16_t *pcm;
  size_t pcm_cap, pcm_frames;
  int arate;
  double apos; /* next frame to play */
  volatile int audio_go;
} V = {.vs = -1, .as = -1, .shown = -1};

/* ------------------------------------------------------------ the file */
static int io_read(void *opaque, uint8_t *buf, int n) {
  (void)opaque;
  const int64_t left = V.len - V.pos;
  if (left <= 0)
    return AVERROR_EOF;
  if (n > left)
    n = (int)left;
  if (fseeko(V.f, (off_t)(V.base + (uint64_t)V.pos), SEEK_SET) != 0)
    return AVERROR(EIO);
  size_t got = fread(buf, 1, (size_t)n, V.f);
  if (!got)
    return AVERROR_EOF;
  V.pos += (int64_t)got;
  return (int)got;
}

static int64_t io_seek(void *opaque, int64_t off, int whence) {
  (void)opaque;
  if (whence == AVSEEK_SIZE)
    return V.len;
  whence &= ~AVSEEK_FORCE;
  int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? V.pos + off : V.len + off;
  if (p < 0 || p > V.len)
    return -1;
  V.pos = p;
  return p;
}

/* ------------------------------------------------------------ pictures */
/* BT.601 (video range) YUV 4:2:0 -> RGBA, 16 pixels a step */
static void yuv_to_rgba(const AVFrame *f, uint8_t *out, int w, int h) {
  const int16x8_t k74 = vdupq_n_s16(74), k102 = vdupq_n_s16(102), k25 = vdupq_n_s16(25), k52 = vdupq_n_s16(52),
                  k129 = vdupq_n_s16(129), k16 = vdupq_n_s16(16), k128 = vdupq_n_s16(128);
  for (int y = 0; y < h; y++) {
    const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
    const uint8_t *pu = f->data[1] + (size_t)(y / 2) * f->linesize[1];
    const uint8_t *pv = f->data[2] + (size_t)(y / 2) * f->linesize[2];
    uint8_t *d = out + (size_t)y * w * 4;
    int x = 0;
    for (; x + 16 <= w; x += 16) {
      const uint8x16_t yy = vld1q_u8(py + x);
      const uint8x8_t uu = vld1_u8(pu + x / 2), vv = vld1_u8(pv + x / 2);
      const uint8x8x2_t u2 = vzip_u8(uu, uu), v2 = vzip_u8(vv, vv);
      uint8x16x4_t px;
      for (int half = 0; half < 2; half++) {
        const uint8x8_t y8 = half ? vget_high_u8(yy) : vget_low_u8(yy);
        const int16x8_t c = vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(y8)), k16), k74);
        const int16x8_t d_ = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u2.val[half])), k128);
        const int16x8_t e = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v2.val[half])), k128);
        const int16x8_t r = vqaddq_s16(c, vmulq_s16(e, k102));
        const int16x8_t g = vqsubq_s16(vqsubq_s16(c, vmulq_s16(d_, k25)), vmulq_s16(e, k52));
        const int16x8_t b = vqaddq_s16(c, vmulq_s16(d_, k129));
        const uint8x8_t r8 = vqrshrun_n_s16(r, 6), g8 = vqrshrun_n_s16(g, 6), b8 = vqrshrun_n_s16(b, 6);
        if (half) {
          px.val[0] = vcombine_u8(vget_low_u8(px.val[0]), r8);
          px.val[1] = vcombine_u8(vget_low_u8(px.val[1]), g8);
          px.val[2] = vcombine_u8(vget_low_u8(px.val[2]), b8);
        } else {
          px.val[0] = vcombine_u8(r8, r8);
          px.val[1] = vcombine_u8(g8, g8);
          px.val[2] = vcombine_u8(b8, b8);
        }
      }
      px.val[3] = vdupq_n_u8(255);
      vst4q_u8(d + x * 4, px);
    }
    for (; x < w; x++) {
      const int c = (py[x] - 16) * 74, du = pu[x / 2] - 128, ev = pv[x / 2] - 128;
      int r = (c + 102 * ev + 32) >> 6, g = (c - 25 * du - 52 * ev + 32) >> 6, b = (c + 129 * du + 32) >> 6;
      d[x * 4 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
      d[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
      d[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
      d[x * 4 + 3] = 255;
    }
  }
}

static void put_picture(const AVFrame *f) {
  if (f->format != AV_PIX_FMT_YUV420P || f->width != V.w || f->height != V.h)
    return;
  int slot = -1;
  while (!V.stop) {
    mutexLock(&V.lock);
    for (int i = 0; i < NSLOT && slot < 0; i++)
      if (!V.ready[i] && i != V.shown)
        slot = i;
    mutexUnlock(&V.lock);
    if (slot >= 0)
      break;
    svcSleepThread(3000000ll);
  }
  if (slot < 0)
    return;
  yuv_to_rgba(f, V.rgba[slot], V.w, V.h);
  const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  mutexLock(&V.lock);
  V.pts[slot] = ts == AV_NOPTS_VALUE ? 0 : (double)ts * V.vtb;
  V.ready[slot] = 1;
  mutexUnlock(&V.lock);
}

/* ------------------------------------------------------------ sound */
static void put_sound(const AVFrame *f) {
  if (!V.pcm || f->nb_samples <= 0)
    return;
  const int ch = f->ch_layout.nb_channels;
  size_t n = (size_t)f->nb_samples;
  mutexLock(&V.lock);
  if (V.pcm_frames + n > V.pcm_cap)
    n = V.pcm_cap - V.pcm_frames;
  int16_t *d = V.pcm + V.pcm_frames * 2;
  mutexUnlock(&V.lock);
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < 2; c++) {
      const int k = c < ch ? c : 0;
      float s;
      switch (f->format) {
      case AV_SAMPLE_FMT_FLTP: s = ((const float *)f->extended_data[k])[i]; break;
      case AV_SAMPLE_FMT_FLT: s = ((const float *)f->extended_data[0])[i * ch + k]; break;
      case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)f->extended_data[k])[i] / 32768.0f; break;
      case AV_SAMPLE_FMT_S16: s = ((const int16_t *)f->extended_data[0])[i * ch + k] / 32768.0f; break;
      default: s = 0; break;
      }
      int v = (int)(s * 32767.0f);
      d[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
  mutexLock(&V.lock);
  V.pcm_frames += n;
  mutexUnlock(&V.lock);
}

/* Into the game's output (48 kHz stereo s16), from ssr_audio.c's mixer. */
void ssr_video_mix(int16_t *out, int frames, int out_rate) {
  if (!V.audio_go || !V.pcm || V.arate <= 0)
    return;
  mutexLock(&V.lock);
  const double step = (double)V.arate / (double)out_rate;
  const double have = (double)V.pcm_frames;
  for (int i = 0; i < frames && V.apos + 1 < have; i++) {
    const int i0 = (int)V.apos;
    const double t = V.apos - i0;
    for (int c = 0; c < 2; c++) {
      const double s = V.pcm[i0 * 2 + c] * (1 - t) + V.pcm[(i0 + 1) * 2 + c] * t;
      int v = out[i * 2 + c] + (int)s;
      out[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    V.apos += step;
  }
  mutexUnlock(&V.lock);
}

/* ------------------------------------------------------------ decoding */
static void decode_thread(void *arg) {
  (void)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  int got_video = 0;
  while (pkt && fr && !V.stop) {
    const int r = av_read_frame(V.fmt, pkt);
    AVCodecContext *dec = NULL;
    if (r >= 0)
      dec = pkt->stream_index == V.vs ? V.vdec : pkt->stream_index == V.as ? V.adec : NULL;
    if (r < 0) { /* the end: what the decoders hold */
      if (V.vdec)
        avcodec_send_packet(V.vdec, NULL);
      while (V.vdec && !V.stop && avcodec_receive_frame(V.vdec, fr) == 0)
        put_picture(fr), got_video++;
      if (V.adec)
        avcodec_send_packet(V.adec, NULL);
      while (V.adec && avcodec_receive_frame(V.adec, fr) == 0)
        put_sound(fr);
      break;
    }
    if (dec && avcodec_send_packet(dec, pkt) >= 0)
      while (!V.stop && avcodec_receive_frame(dec, fr) == 0) {
        if (dec == V.vdec)
          put_picture(fr), got_video++;
        else
          put_sound(fr);
      }
    av_packet_unref(pkt);
  }
  av_frame_free(&fr);
  av_packet_free(&pkt);
  debugPrintf("[video] decoded %d pictures, %u sound frames%s\n", got_video, (unsigned)V.pcm_frames,
              V.stop ? " (stopped)" : "");
  V.eof = 1;
}

static void close_all(void) {
  V.stop = 1;
  if (V.thread_on) {
    threadWaitForExit(&V.thread);
    threadClose(&V.thread);
    V.thread_on = 0;
  }
  mutexLock(&V.lock);
  V.audio_go = 0;
  mutexUnlock(&V.lock);
  avcodec_free_context(&V.vdec);
  avcodec_free_context(&V.adec);
  if (V.fmt)
    avformat_close_input(&V.fmt);
  if (V.io) {
    av_freep(&V.io->buffer);
    avio_context_free(&V.io);
  }
  for (int i = 0; i < NSLOT; i++) {
    free(V.rgba[i]);
    V.rgba[i] = NULL;
    V.ready[i] = 0;
  }
  free(V.pcm);
  V.pcm = NULL;
  if (V.f)
    fclose(V.f);
  V.f = NULL;
  if (V.playing)
    dcr_boost_hold(0);
  V.open = V.playing = 0;
  V.shown = -1;
  V.vs = V.as = -1;
}

static AVCodecContext *open_decoder(AVStream *st) {
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *ctx = c ? avcodec_alloc_context3(c) : NULL;
  if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, c, NULL) < 0) {
    avcodec_free_context(&ctx);
    return NULL;
  }
  return ctx;
}

static int open_movie(void) {
  const SsrPack *p = ssr_pack();
  if (!p || !p->video_len) {
    debugPrintf("[video] no intro_full.m4v next to the game's data\n");
    return 0;
  }
  mutexInit(&V.lock);
  V.stop = V.skip = V.eof = 0;
  V.finished = V.started = 0;
  V.apos = 0;
  V.pcm_frames = 0;
  V.f = fopen(p->path, "rb");
  if (!V.f) {
    debugPrintf("[video] %s: cannot open it\n", p->path);
    return 0;
  }
  setvbuf(V.f, NULL, _IOFBF, 64 * 1024);
  V.base = p->video_off;
  V.len = p->video_len;
  V.pos = 0;
  av_log_set_level(AV_LOG_ERROR);
  unsigned char *buf = av_malloc(32768);
  V.io = buf ? avio_alloc_context(buf, 32768, 0, NULL, io_read, NULL, io_seek) : NULL;
  V.fmt = avformat_alloc_context();
  if (!V.io || !V.fmt) {
    close_all();
    return 0;
  }
  V.fmt->pb = V.io;
  if (avformat_open_input(&V.fmt, NULL, NULL, NULL) < 0 || avformat_find_stream_info(V.fmt, NULL) < 0) {
    debugPrintf("[video] intro_full.m4v: not a video FFmpeg reads here\n");
    close_all();
    return 0;
  }
  V.vs = av_find_best_stream(V.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  V.as = av_find_best_stream(V.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (V.vs >= 0)
    V.vdec = open_decoder(V.fmt->streams[V.vs]);
  if (V.as >= 0)
    V.adec = open_decoder(V.fmt->streams[V.as]);
  if (!V.vdec) {
    debugPrintf("[video] intro_full.m4v: no picture decoder for it\n");
    close_all();
    return 0;
  }
  V.w = V.vdec->width;
  V.h = V.vdec->height;
  V.vtb = av_q2d(V.fmt->streams[V.vs]->time_base);
  for (int i = 0; i < NSLOT; i++)
    if (!(V.rgba[i] = memalign(64, (size_t)V.w * V.h * 4))) {
      close_all();
      return 0;
    }
  if (V.adec) {
    V.arate = V.adec->sample_rate;
    const double secs = V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 60.0;
    V.pcm_cap = (size_t)((secs + 2.0) * V.arate);
    V.pcm = malloc(V.pcm_cap * 4);
  }
  V.open = 1;
  debugPrintf("[video] intro_full.m4v: %dx%d %s, %s, %.1f s\n", V.w, V.h, avcodec_get_name(V.vdec->codec_id),
              V.adec ? avcodec_get_name(V.adec->codec_id) : "no sound",
              V.fmt->duration > 0 ? (double)V.fmt->duration / AV_TIME_BASE : 0.0);
  return 1;
}

int ssr_video_start(void) {
  if (V.playing)
    return 1;
  if (!V.open && !open_movie())
    return 0;
  V.stop = 0;
  /* a core of its own, as the game's threads: the decoder runs ahead */
  if (R_FAILED(threadCreate(&V.thread, decode_thread, NULL, NULL, 0x40000, 0x2C, -2)) ||
      R_FAILED(threadStart(&V.thread))) {
    debugPrintf("[video] no thread for the decoder\n");
    close_all();
    return 0;
  }
  V.thread_on = 1;
  V.playing = 1;
  dcr_boost_hold(1);
  return 1;
}

int ssr_video_playing(void) { return V.playing; }

void ssr_video_skip(void) {
  if (V.playing && !V.skip) {
    V.skip = 1;
    debugPrintf("[video] skipped\n");
  }
}

/* ------------------------------------------------------------ drawing */
typedef void (*t_geti)(GLenum, GLint *);
typedef void (*t_bind)(GLenum, GLuint);
typedef void (*t_active)(GLenum);
typedef void (*t_gen)(GLsizei, GLuint *);
typedef void (*t_del)(GLsizei, const GLuint *);
typedef void (*t_teximage)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*t_texsub)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*t_texparami)(GLenum, GLenum, GLint);
typedef void (*t_pixelstorei)(GLenum, GLint);
typedef GLenum (*t_geterror)(void);

static struct {
  t_geti GetIntegerv;
  t_bind BindTexture;
  t_active ActiveTexture;
  t_gen GenTextures;
  t_del DeleteTextures;
  t_teximage TexImage2D;
  t_texsub TexSubImage2D;
  t_texparami TexParameteri;
  t_pixelstorei PixelStorei;
  t_geterror GetError;
} G;
static GLuint g_tex;
static int g_tex_w, g_tex_h;
static int g_gl; /* 0 not looked up, 1 ready, -1 unavailable */

#define L(name) (G.name = (void *)dcr_gl_lookup("gl" #name))
static int gl_setup(void) {
  L(GetIntegerv), L(BindTexture), L(ActiveTexture), L(GenTextures), L(DeleteTextures), L(TexImage2D);
  L(TexSubImage2D), L(TexParameteri), L(PixelStorei), L(GetError);
  if (!G.GetIntegerv || !G.BindTexture || !G.GenTextures || !G.TexImage2D || !G.TexSubImage2D ||
      !G.TexParameteri || !G.PixelStorei || !G.GetError || dcr_blit_setup("video") < 0) {
    debugPrintf("[video] the GL driver lacks a basic call: no pictures\n");
    return -1;
  }
  return 1;
}

static void finish(const char *why) {
  if (!V.playing)
    return;
  debugPrintf("[video] ended (%s)\n", why);
  close_all();
  V.finished = 1;
  if (g_tex && G.DeleteTextures) {
    G.DeleteTextures(1, &g_tex);
    g_tex = 0;
    g_tex_w = g_tex_h = 0;
  }
}

static double now_s(void) { return (double)armTicksToNs(armGetSystemTick() - V.t0) / 1e9; }

/* Before eglSwapBuffers, on the engine's thread: the picture due, over the
 * whole window (the movie is 16:9, as the screen). */
void ssr_video_draw(void) {
  if (!V.playing)
    return;
  if (V.skip) {
    finish("skipped");
    return;
  }
  if (!g_gl)
    g_gl = gl_setup();
  if (g_gl < 0) {
    finish("no GL");
    return;
  }

  mutexLock(&V.lock);
  int due = -1;
  if (!V.started) {
    double first = 1e9;
    for (int i = 0; i < NSLOT; i++)
      if (V.ready[i] && V.pts[i] < first)
        first = V.pts[i], due = i;
    if (due >= 0) {
      V.started = 1;
      V.t0 = armGetSystemTick() - armNsToTicks((u64)(first * 1e9));
      V.audio_go = 1;
    }
  } else {
    const double t = now_s();
    for (int i = 0; i < NSLOT; i++)
      if (V.ready[i] && V.pts[i] <= t && (due < 0 || V.pts[i] > V.pts[due]))
        due = i;
  }
  int any_left = 0;
  if (due >= 0) {
    for (int i = 0; i < NSLOT; i++) /* the ones it passed are done with */
      if (V.ready[i] && i != due && V.pts[i] < V.pts[due])
        V.ready[i] = 0;
    if (V.shown >= 0 && V.shown != due)
      V.ready[V.shown] = 0;
  }
  for (int i = 0; i < NSLOT; i++)
    any_left |= V.ready[i] && i != due && i != V.shown;
  mutexUnlock(&V.lock);

  if (due >= 0 && due != V.shown) { /* into the texture */
    GLint bound = 0, align = 4, active = GL_TEXTURE0;
    if (G.ActiveTexture) {
      G.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
      G.ActiveTexture(GL_TEXTURE0);
    }
    G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    G.GetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    while (G.GetError() != GL_NO_ERROR)
      ;
    if (!g_tex) {
      G.GenTextures(1, &g_tex);
      G.BindTexture(GL_TEXTURE_2D, g_tex);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      G.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    G.BindTexture(GL_TEXTURE_2D, g_tex);
    G.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (g_tex_w != V.w || g_tex_h != V.h) {
      G.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, V.w, V.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, V.rgba[due]);
      debugPrintf("[video] texture %dx%d (texture %u): GL error 0x%x\n", V.w, V.h, g_tex, (unsigned)G.GetError());
      g_tex_w = V.w, g_tex_h = V.h;
    } else {
      G.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V.w, V.h, GL_RGBA, GL_UNSIGNED_BYTE, V.rgba[due]);
    }
    G.PixelStorei(GL_UNPACK_ALIGNMENT, align);
    G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
    if (G.ActiveTexture)
      G.ActiveTexture((GLenum)active);
    V.shown = due;
    V.shown_pts = V.pts[due];
  }
  if (V.shown >= 0) {
    int vw, vh;
    dcr_window_size(&vw, &vh);
    dcr_blit(g_tex, 0, 0, vw, vh, 0, 0);
  }

  /* the end: everything decoded and shown, and the last picture's time (a
   * frame) past */
  if (V.eof && !any_left && V.started && now_s() > V.shown_pts + 0.1)
    finish("the end");
}
#endif /* DCR_VIDEO */
