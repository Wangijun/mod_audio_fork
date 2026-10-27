#include "downstream_pcm.h"

#include <speex/speex_resampler.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct af_pcm {
  SpeexResamplerState *resampler;
  uint32_t source_rate, output_rate, channels;
  int quality, ended, error;
  uint8_t partial[4];
  size_t partial_len;
  uint64_t input_frames, output_frames;
};

int af_pcm_reset(af_pcm_t *p, uint32_t source, uint32_t output, uint32_t channels) {
  int error = 0;
  if (!p) return AF_PCM_FORMAT;
  if (p->resampler) speex_resampler_destroy(p->resampler);
  p->resampler = NULL;
  p->ended = p->error = 0;
  p->partial_len = 0;
  p->input_frames = p->output_frames = 0;
  if (source < 8000 || source > 64000 || output < 8000 || output > 192000 ||
      channels < 1 || channels > 2) return p->error = AF_PCM_FORMAT;
  p->source_rate = source;
  p->output_rate = output;
  p->channels = channels;
  if (source != output) {
    p->resampler = speex_resampler_init(channels, source, output, p->quality, &error);
    if (!p->resampler || error || speex_resampler_skip_zeros(p->resampler))
      return p->error = AF_PCM_RESAMPLE;
  }
  return AF_PCM_OK;
}

af_pcm_t *af_pcm_create(uint32_t source, uint32_t output, uint32_t channels, int quality) {
  af_pcm_t *p = (af_pcm_t *)calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->quality = quality;
  if (af_pcm_reset(p, source, output, channels)) af_pcm_destroy(&p);
  return p;
}

void af_pcm_destroy(af_pcm_t **p) {
  if (!p || !*p) return;
  if ((*p)->resampler) speex_resampler_destroy((*p)->resampler);
  free(*p);
  *p = NULL;
}

static int emit(af_pcm_t *p, const int16_t *samples, size_t frames,
                af_pcm_sink_t sink, void *user) {
  if (frames && sink(user, samples, frames * p->channels * sizeof(int16_t)))
    return p->error = AF_PCM_SINK;
  p->output_frames += frames;
  return AF_PCM_OK;
}

static int convert(af_pcm_t *p, const int16_t *input, uint32_t frames,
                   uint64_t limit, af_pcm_sink_t sink, void *user) {
  int16_t output[2048];
  if (!p->resampler) return emit(p, input, frames, sink, user);
  while (frames) {
    spx_uint32_t consumed = frames, produced = 1024;
    if (speex_resampler_process_interleaved_int(p->resampler, input, &consumed,
                                               output, &produced) || (!consumed && !produced))
      return p->error = AF_PCM_RESAMPLE;
    if (produced > 1024 || consumed > frames) return p->error = AF_PCM_RESAMPLE;
    size_t keep = produced;
    if (p->output_frames >= limit) keep = 0;
    else if (keep > limit - p->output_frames) keep = (size_t)(limit - p->output_frames);
    if (emit(p, output, keep, sink, user)) return p->error;
    frames -= consumed;
    input += consumed * p->channels;
  }
  return AF_PCM_OK;
}

int af_pcm_write(af_pcm_t *p, const void *data, size_t bytes, af_pcm_sink_t sink, void *user) {
  const uint8_t *cursor = (const uint8_t *)data;
  int16_t input[512];
  size_t frame_bytes;
  if (!p || !sink || (bytes && !data)) return AF_PCM_FORMAT;
  if (p->error) return p->error;
  if (p->ended) return bytes ? AF_PCM_ENDED : AF_PCM_OK;
  frame_bytes = p->channels * sizeof(int16_t);
  while (bytes) {
    size_t frames = 0;
    while (bytes && frames < 256) {
      size_t take = frame_bytes - p->partial_len;
      if (take > bytes) take = bytes;
      memcpy(p->partial + p->partial_len, cursor, take);
      p->partial_len += take;
      cursor += take;
      bytes -= take;
      if (p->partial_len != frame_bytes) break;
      for (uint32_t ch = 0; ch < p->channels; ++ch) {
        input[frames * p->channels + ch] = (int16_t)((uint16_t)p->partial[ch * 2] |
                                                       (uint16_t)p->partial[ch * 2 + 1] << 8);
      }
      p->partial_len = 0;
      ++frames;
    }
    if (p->input_frames > (UINT64_MAX - p->source_rate) / p->output_rate - frames)
      return p->error = AF_PCM_FORMAT;
    p->input_frames += frames;
    if (convert(p, input, (uint32_t)frames, UINT64_MAX, sink, user)) return p->error;
  }
  return AF_PCM_OK;
}

int af_pcm_finish(af_pcm_t *p, af_pcm_sink_t sink, void *user) {
  uint64_t target;
  if (!p || !sink) return AF_PCM_FORMAT;
  if (p->error || p->ended) return p->error;
  p->ended = 1;
  if (p->partial_len) return p->error = AF_PCM_PARTIAL;
  target = (p->input_frames * p->output_rate + p->source_rate - 1) / p->source_rate;
  if (p->resampler && p->output_frames < target) {
    /* skip_zeros 已移除起始群延迟；最多送入一个滤波延迟和取整余量的零输入。 */
    uint32_t remaining = speex_resampler_get_input_latency(p->resampler) +
                         (p->source_rate + p->output_rate - 1) / p->output_rate + 2;
    int16_t zeros[512] = {0};
    while (remaining && p->output_frames < target) {
      uint32_t n = remaining > 256 ? 256 : remaining;
      if (convert(p, zeros, n, target, sink, user)) return p->error;
      remaining -= n;
    }
  }
  if (p->output_frames != target) return p->error = AF_PCM_RESAMPLE;
  return AF_PCM_OK;
}

const char *af_pcm_error(int error) {
  switch (error) {
    case AF_PCM_OK: return "ok";
    case AF_PCM_FORMAT: return "pcm_invalid_format";
    case AF_PCM_RESAMPLE: return "pcm_resample_failed";
    case AF_PCM_SINK: return "pcm_buffer_overflow";
    case AF_PCM_PARTIAL: return "pcm_partial_frame";
    case AF_PCM_ENDED: return "pcm_already_ended";
    default: return "pcm_unknown_error";
  }
}

uint64_t af_monotonic_us(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

int af_watchdog_expired(af_watchdog_t *w, uint64_t serial, int received,
                        int missing, uint32_t timeout_ms) {
  uint64_t now = af_monotonic_us();
  if (received || serial != w->seen_audio) w->since_us = 0;
  w->seen_audio = serial;
  if (!missing) { w->since_us = 0; return 0; }
  if (!w->since_us) w->since_us = now;
  return timeout_ms && now - w->since_us >= (uint64_t)timeout_ms * 1000U;
}
