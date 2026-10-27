#include "downstream_pcm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
typedef struct { int16_t samples[262144]; size_t bytes; } capture_t;
static int capture(void *user, const int16_t *pcm, size_t bytes) {
  capture_t *c = user;
  CHECK(bytes <= sizeof(c->samples) - c->bytes);
  memcpy((char *)c->samples + c->bytes, pcm, bytes);
  c->bytes += bytes;
  return 0;
}
static int reject(void *user, const int16_t *pcm, size_t bytes) {
  (void)user; (void)pcm; (void)bytes; return -1;
}
static void render(const int16_t *input, size_t frames, uint32_t source, uint32_t output,
                   uint32_t channels, int fragmented, capture_t *c) {
  af_pcm_t *p = af_pcm_create(source, output, channels, 2);
  CHECK(p);
  size_t bytes = frames * channels * 2, offset = 0;
  while (offset < bytes) {
    size_t n = fragmented ? (offset % 103 + 1) : bytes;
    if (n > bytes - offset) n = bytes - offset;
    CHECK(!af_pcm_write(p, (const char *)input + offset, n, capture, c));
    offset += n;
  }
  CHECK(!af_pcm_finish(p, capture, c));
  size_t before = c->bytes;
  CHECK(!af_pcm_finish(p, capture, c) && c->bytes == before);
  CHECK(c->bytes == ((frames * output + source - 1) / source) * channels * 2);
  af_pcm_destroy(&p);
  CHECK(!p);
}
static void format_case(uint32_t source, uint32_t output, uint32_t channels, size_t frames) {
  int16_t input[48000 * 2] = {0};
  capture_t *whole = calloc(1, sizeof(*whole)), *pieces = calloc(1, sizeof(*pieces));
  CHECK(whole && pieces);
  for (size_t i = 0; i < frames; ++i)
    for (uint32_t ch = 0; ch < channels; ++ch)
      input[i * channels + ch] = (int16_t)(9000 * sin(2 * 3.141592653589793 * (ch ? 900 : 500) * i / source));
  if (frames > 128) input[(frames - 32) * channels] = 28000;
  render(input, frames, source, output, channels, 0, whole);
  render(input, frames, source, output, channels, 1, pieces);
  CHECK(whole->bytes == pieces->bytes);
  CHECK(!memcmp(whole->samples, pieces->samples, whole->bytes));
  if (source == output) CHECK(!memcmp(input, whole->samples, whole->bytes));
  printf("%u -> %u, channels=%u, input=%zu, output=%zu: aligned/chunked equal\n",
         source, output, channels, frames, whole->bytes / channels / 2);
  free(whole); free(pieces);
}
static double tone_energy(double frequency) {
  int16_t input[24000];
  capture_t *c = calloc(1, sizeof(*c));
  CHECK(c);
  for (size_t i = 0; i < 24000; ++i) input[i] = (int16_t)(12000 * sin(2 * 3.141592653589793 * frequency * i / 24000));
  render(input, 24000, 24000, 8000, 1, 1, c);
  double energy = 0;
  for (size_t i = 100; i < 7900; ++i) energy += (double)c->samples[i] * c->samples[i];
  free(c);
  return energy / 7800;
}
static void tail_and_reset(void) {
  int16_t input[1600] = {0};
  input[1580] = 30000;
  capture_t *c = calloc(1, sizeof(*c)), *fresh = calloc(1, sizeof(*fresh));
  CHECK(c && fresh);
  af_pcm_t *p = af_pcm_create(16000, 8000, 1, 2);
  CHECK(p);
  CHECK(!af_pcm_write(p, input, sizeof(input), capture, c));
  size_t before = c->bytes;
  CHECK(!af_pcm_finish(p, capture, c));
  CHECK(c->bytes == 1600 && before < c->bytes);
  int peak = 0;
  for (size_t i = 780; i < 800; ++i) if (abs(c->samples[i]) > peak) peak = abs(c->samples[i]);
  CHECK(peak > 5000);
  CHECK(af_pcm_write(p, input, 2, capture, c) == AF_PCM_ENDED);
  CHECK(!af_pcm_reset(p, 16000, 8000, 1));
  c->bytes = 0;
  CHECK(!af_pcm_write(p, input, 1, capture, c));
  CHECK(af_pcm_finish(p, capture, c) == AF_PCM_PARTIAL);
  CHECK(af_pcm_finish(p, capture, c) == AF_PCM_PARTIAL);
  CHECK(!af_pcm_reset(p, 16000, 8000, 1));
  CHECK(!af_pcm_write(p, input, 135, capture, c));
  CHECK(!af_pcm_reset(p, 16000, 8000, 1)); /* 打断丢弃旧滤波历史与半采样 */
  c->bytes = 0;
  CHECK(!af_pcm_write(p, input, sizeof(input), capture, c));
  CHECK(!af_pcm_finish(p, capture, c));
  render(input, 1600, 16000, 8000, 1, 0, fresh);
  CHECK(c->bytes == fresh->bytes && !memcmp(c->samples, fresh->samples, c->bytes));
  CHECK(!af_pcm_reset(p, 8000, 8000, 1));
  CHECK(af_pcm_write(p, input, 2, reject, NULL) == AF_PCM_SINK);
  CHECK(af_pcm_finish(p, capture, c) == AF_PCM_SINK);
  CHECK(af_pcm_reset(p, 1, 8000, 1) == AF_PCM_FORMAT);
  af_pcm_destroy(&p);
  free(c); free(fresh);
}
int main(void) {
  format_case(8000, 8000, 1, 8000);
  format_case(16000, 8000, 1, 16001);
  format_case(24000, 8000, 1, 24000);
  format_case(8000, 16000, 1, 8000);
  format_case(8000, 64000, 1, 8000);
  format_case(64000, 8000, 2, 40000);
  format_case(16000, 8000, 2, 16000);
  format_case(8000, 16000, 2, 41);
  for (size_t n = 0; n < 40; ++n) {
    format_case(24000, 8000, 1, n);
    format_case(8000, 16000, 1, n);
  }
  double pass = tone_energy(1000), stop = tone_energy(6500);
  CHECK(stop / pass < 0.005);
  printf("alias suppression: %.1f dB\n", 10 * log10(stop / pass));
  tail_and_reset();
  af_watchdog_t w = {0};
  CHECK(!af_watchdog_expired(&w, 0, 0, 1, 30));
  for (int i = 0; i < 100000; ++i) CHECK(!af_watchdog_expired(&w, 0, 0, 1, 100));
  struct timespec delay = {0, 40000000};
  nanosleep(&delay, NULL);
  CHECK(af_watchdog_expired(&w, 0, 0, 1, 30));
  CHECK(!af_watchdog_expired(&w, 1, 0, 1, 30));
  puts("PCM tail, reset, errors and monotonic watchdog: passed");
  return 0;
}
