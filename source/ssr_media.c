/* ssr_media.c -- MP3 decoding for the music (FFmpeg).
 *
 * The music is res/raw/music_*.mp3 in the APK, stored (uncompressed), so a
 * track is read straight out of the APK at its entry's offset: a few KB at a
 * time, parsed into MPEG audio frames (FFmpeg's mpegaudio parser) and
 * decoded (its fixed-point MP3 decoder) into interleaved stereo s16 at the
 * file's own rate (ssr_audio.c resamples). An ID3v2 tag at the start is
 * skipped. FFmpeg is ffmpeg32's LGPL build (portlibs32/:
 * the MOV demuxer, the H.264, AAC and MP3 decoders).
 *
 * This file is compiled with -fno-short-enums, as FFmpeg is; its interface
 * (ssr.h) has no enums. MIT.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "ssr.h"
#include "util.h"

#if !DCR_VIDEO /* built without FFmpeg: no music */
SsrMp3 *ssr_mp3_open(const char *path, uint64_t off, uint32_t len) {
  (void)path, (void)off, (void)len;
  return NULL;
}
int ssr_mp3_read(SsrMp3 *m, int16_t *out, int max) { return 0; }
int ssr_mp3_rate(const SsrMp3 *m) { return 44100; }
void ssr_mp3_rewind(SsrMp3 *m) {}
void ssr_mp3_close(SsrMp3 *m) {}
#else

#include <libavcodec/avcodec.h>

#define IN_CHUNK 8192
#define PCM_CAP (1152 * 8) /* frames held decoded ahead */

struct SsrMp3 {
  FILE *f;
  uint64_t start, end, pos; /* the audio data in the file (past the ID3 tag) */
  const AVCodec *codec;
  AVCodecParserContext *parser;
  AVCodecContext *ctx;
  AVPacket *pkt;
  AVFrame *frame;
  uint8_t in[IN_CHUNK + AV_INPUT_BUFFER_PADDING_SIZE];
  int in_len, in_off;
  int16_t pcm[PCM_CAP * 2];
  int pcm_n;
  int rate;
  int eof, flushed;
};

static size_t id3_size(FILE *f, uint64_t off, uint32_t len) {
  uint8_t h[10];
  if (len < 10 || fseeko(f, (off_t)off, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10)
    return 0;
  if (memcmp(h, "ID3", 3))
    return 0;
  size_t sz = ((size_t)(h[6] & 0x7f) << 21) | ((size_t)(h[7] & 0x7f) << 14) | ((size_t)(h[8] & 0x7f) << 7) |
              (size_t)(h[9] & 0x7f);
  sz += 10 + ((h[5] & 0x10) ? 10 : 0); /* the footer, when flagged */
  return sz < len ? sz : 0;
}

static int open_codec(SsrMp3 *m) {
  m->parser = av_parser_init(AV_CODEC_ID_MP3);
  m->ctx = avcodec_alloc_context3(m->codec);
  if (!m->parser || !m->ctx || avcodec_open2(m->ctx, m->codec, NULL) < 0)
    return -1;
  return 0;
}

static void close_codec(SsrMp3 *m) {
  if (m->parser)
    av_parser_close(m->parser);
  m->parser = NULL;
  avcodec_free_context(&m->ctx);
}

SsrMp3 *ssr_mp3_open(const char *path, uint64_t off, uint32_t len) {
  static int logged;
  SsrMp3 *m = calloc(1, sizeof *m);
  if (!m)
    return NULL;
  m->codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
  m->f = fopen(path, "rb");
  m->pkt = av_packet_alloc();
  m->frame = av_frame_alloc();
  if (!m->codec || !m->f || !m->pkt || !m->frame || open_codec(m) != 0) {
    if (!logged++)
      debugPrintf("[music] the MP3 decoder could not be opened (%s)\n", m->codec ? "context" : "no decoder");
    ssr_mp3_close(m);
    return NULL;
  }
  setvbuf(m->f, NULL, _IOFBF, 64 * 1024);
  m->start = off + id3_size(m->f, off, len);
  m->end = off + len;
  m->pos = m->start;
  m->rate = 44100;
  return m;
}

void ssr_mp3_close(SsrMp3 *m) {
  if (!m)
    return;
  close_codec(m);
  av_packet_free(&m->pkt);
  av_frame_free(&m->frame);
  if (m->f)
    fclose(m->f);
  free(m);
}

void ssr_mp3_rewind(SsrMp3 *m) {
  close_codec(m);
  open_codec(m);
  m->pos = m->start;
  m->in_len = m->in_off = 0;
  m->pcm_n = 0;
  m->eof = m->flushed = 0;
}

int ssr_mp3_rate(const SsrMp3 *m) { return m ? m->rate : 44100; }

/* Every frame the decoder has ready, appended to pcm[] as interleaved s16. */
static void drain(SsrMp3 *m) {
  while (avcodec_receive_frame(m->ctx, m->frame) == 0) {
    const AVFrame *fr = m->frame;
    int ch = fr->ch_layout.nb_channels > 0 ? fr->ch_layout.nb_channels : 1;
    if (fr->sample_rate > 0)
      m->rate = fr->sample_rate;
    int n = fr->nb_samples;
    if (n > PCM_CAP - m->pcm_n)
      n = PCM_CAP - m->pcm_n;
    int16_t *o = m->pcm + m->pcm_n * 2;
    for (int i = 0; i < n; i++) {
      int l, r;
      switch (fr->format) {
      case AV_SAMPLE_FMT_S16P:
        l = ((const int16_t *)fr->data[0])[i];
        r = ch > 1 ? ((const int16_t *)fr->data[1])[i] : l;
        break;
      case AV_SAMPLE_FMT_S16:
        l = ((const int16_t *)fr->data[0])[i * ch];
        r = ch > 1 ? ((const int16_t *)fr->data[0])[i * ch + 1] : l;
        break;
      case AV_SAMPLE_FMT_FLTP: {
        float a = ((const float *)fr->data[0])[i], b = ch > 1 ? ((const float *)fr->data[1])[i] : a;
        l = (int)(a * 32767.0f), r = (int)(b * 32767.0f);
        break;
      }
      case AV_SAMPLE_FMT_FLT: {
        float a = ((const float *)fr->data[0])[i * ch], b = ch > 1 ? ((const float *)fr->data[0])[i * ch + 1] : a;
        l = (int)(a * 32767.0f), r = (int)(b * 32767.0f);
        break;
      }
      default:
        l = r = 0;
        break;
      }
      o[i * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
      o[i * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
    m->pcm_n += n;
  }
}

int ssr_mp3_read(SsrMp3 *m, int16_t *out, int max) {
  if (!m || !m->ctx)
    return 0;
  while (m->pcm_n < max && !m->flushed) {
    if (m->in_off >= m->in_len && !m->eof) {
      size_t want = m->end - m->pos < IN_CHUNK ? (size_t)(m->end - m->pos) : IN_CHUNK;
      size_t got = 0;
      if (want && fseeko(m->f, (off_t)m->pos, SEEK_SET) == 0)
        got = fread(m->in, 1, want, m->f);
      m->pos += got;
      m->in_len = (int)got;
      m->in_off = 0;
      memset(m->in + got, 0, AV_INPUT_BUFFER_PADDING_SIZE);
      if (!got)
        m->eof = 1;
    }
    uint8_t *data = NULL;
    int size = 0;
    if (m->in_off < m->in_len) {
      int used = av_parser_parse2(m->parser, m->ctx, &data, &size, m->in + m->in_off, m->in_len - m->in_off,
                                  AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) {
        m->in_off = m->in_len;
        continue;
      }
      m->in_off += used;
    } else if (m->eof) {
      /* the parser's last frame, then the decoder's */
      av_parser_parse2(m->parser, m->ctx, &data, &size, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (!size) {
        avcodec_send_packet(m->ctx, NULL);
        drain(m);
        m->flushed = 1;
        break;
      }
    }
    if (size) {
      m->pkt->data = data;
      m->pkt->size = size;
      if (avcodec_send_packet(m->ctx, m->pkt) == AVERROR(EAGAIN)) {
        drain(m); /* the decoder's output first, then the packet again */
        avcodec_send_packet(m->ctx, m->pkt);
      }
      drain(m);
    }
    if (m->pcm_n >= PCM_CAP - 1152)
      break;
  }
  int n = m->pcm_n < max ? m->pcm_n : max;
  memcpy(out, m->pcm, (size_t)n * 4);
  memmove(m->pcm, m->pcm + n * 2, (size_t)(m->pcm_n - n) * 4);
  m->pcm_n -= n;
  return n;
}

#endif /* DCR_VIDEO */
