#include "audio_fork_http.h"
#include "audio_fork_ws.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
/* 只初始化 APR 内存池，不启动 FreeSWITCH 服务或模块线程。 */
extern int fspr_initialize(void);
extern void fspr_terminate(void);
extern int fspr_pool_create_ex(switch_memory_pool_t **, switch_memory_pool_t *, void *, void *);
extern void fspr_pool_destroy(switch_memory_pool_t *);
uint32_t audio_fork_ws_prebuffer_ms = 200, audio_fork_http_prebuffer_ms = 200;
static unsigned cond_waits;
switch_status_t __wrap_switch_thread_cond_timedwait(switch_thread_cond_t *c, switch_mutex_t *m, switch_interval_time_t t) {
  (void)c; (void)m; (void)t; ++cond_waits; return SWITCH_STATUS_TIMEOUT;
}
void __wrap_switch_log_printf(switch_text_channel_t channel, const char *file, const char *func,
                              int line, const char *userdata, switch_log_level_t level, const char *fmt, ...) {
  (void)channel; (void)file; (void)func; (void)line; (void)userdata; (void)level; (void)fmt;
}

static void ws_context(audio_fork_ws_ctx_t *ctx, private_t *p) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->tech_pvt = p;
  ctx->samplerate = p->downstream_output_rate;
  ctx->channels = p->downstream_channels;
  ctx->generation = p->downstream_generation;
  ctx->prebuffer_bytes = 3200;
  ctx->watchdog_ms = 200;
  ++p->downstream_current_handles;
  switch_atomic_inc(&p->downstream_handles);
}

static void ws_tests(switch_memory_pool_t *pool) {
  private_t p = {0};
  p.downstream_default_rate = 16000;
  p.downstream_output_rate = 8000;
  p.downstream_channels = 1;
  CHECK(switch_mutex_init(&p.downstream_mutex, SWITCH_MUTEX_NESTED, pool) == SWITCH_STATUS_SUCCESS);
  CHECK(switch_buffer_create_dynamic(&p.downstream_buffer, 4096, 16384, 2U * 1024U * 1024U) == SWITCH_STATUS_SUCCESS);
  p.downstream_pcm = af_pcm_create(16000, 8000, 1, SWITCH_RESAMPLE_QUALITY);
  CHECK(p.downstream_pcm);
  CHECK(!audio_fork_ws_start(&p, 0, 0));
  switch_mutex_lock(p.downstream_mutex);
  CHECK(!audio_fork_ws_format_locked(&p, 24000, 1, 8000)); /* URL 在首包前补充 */
  CHECK(audio_fork_ws_format_locked(&p, 24000, 1, 16000) == AF_PCM_FORMAT);
  CHECK(audio_fork_ws_format_locked(&p, 24000, 2, 8000) == AF_PCM_FORMAT);
  switch_mutex_unlock(p.downstream_mutex);
  CHECK(!audio_fork_ws_start(&p, 16000, 1));
  CHECK(audio_fork_ws_format_locked(&p, 24000, 1, 8000) == AF_PCM_FORMAT);
  int16_t input[1600];
  for (size_t i = 0; i < 1600; ++i) input[i] = (int16_t)(i * 13);
  CHECK(!audio_fork_ws_produce(&p, input, 1));
  CHECK(audio_fork_ws_format_locked(&p, 24000, 1, 8000) == AF_PCM_FORMAT);
  CHECK(!audio_fork_ws_produce(&p, (char *)input + 1, sizeof(input) - 1));
  CHECK(!audio_fork_ws_produce(&p, NULL, 0));
  size_t before = switch_buffer_inuse(p.downstream_buffer);
  CHECK(before == 1600);
  CHECK(!audio_fork_ws_produce(&p, NULL, 0));
  CHECK(switch_buffer_inuse(p.downstream_buffer) == before);

  audio_fork_ws_ctx_t ctx;
  ws_context(&ctx, &p); /* EOF 和音频早于句柄，且短于预缓冲 */
  switch_file_handle_t handle = {0};
  handle.private_info = &ctx;
  int16_t output[16384];
  size_t len = 1024;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS && len == 800);
  len = 160;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);
  CHECK(audio_fork_ws_file_close(&handle) == SWITCH_STATUS_SUCCESS);
  CHECK(!switch_buffer_inuse(p.downstream_buffer));

  CHECK(!audio_fork_ws_start(&p, 8000, 1));
  ws_context(&ctx, &p);
  handle.private_info = &ctx;
  ctx.prebuffered = 1;
  CHECK(!audio_fork_ws_produce(&p, input, 40));
  memset(output, 0x77, sizeof(output));
  len = 160;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS && len == 160);
  CHECK(!memcmp(input, output, 40));
  for (size_t i = 20; i < 160; ++i) CHECK(output[i] == 0);
  len = 16384;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS && len == 160);
  ctx.watchdog.since_us = af_monotonic_us() - 201000;
  len = 160;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);

  audio_fork_ws_ctx_t old = ctx;
  CHECK(!audio_fork_ws_start(&p, 8000, 1));
  CHECK(!audio_fork_ws_produce(&p, input, 40));
  before = switch_buffer_inuse(p.downstream_buffer);
  switch_file_handle_t old_handle = {0};
  old_handle.private_info = &old;
  CHECK(audio_fork_ws_file_close(&old_handle) == SWITCH_STATUS_SUCCESS);
  CHECK(switch_buffer_inuse(p.downstream_buffer) == before);
  CHECK(switch_atomic_read(&p.downstream_accept_audio) && !switch_atomic_read(&p.downstream_interrupted));
  handle.private_info = &ctx; /* 旧代际读端也立即失效 */
  len = 160;
  CHECK(audio_fork_ws_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);
  ws_context(&ctx, &p);
  handle.private_info = &ctx;
  CHECK(audio_fork_ws_file_close(&handle) == SWITCH_STATUS_SUCCESS);
  CHECK(!switch_buffer_inuse(p.downstream_buffer));
  CHECK(!switch_atomic_read(&p.downstream_accept_audio));

  CHECK(!audio_fork_ws_start(&p, 16000, 1));
  CHECK(!audio_fork_ws_produce(&p, input, 3));
  CHECK(audio_fork_ws_produce(&p, NULL, 0) == AF_PCM_PARTIAL);
  CHECK(switch_atomic_read(&p.downstream_interrupted));
  CHECK(!switch_buffer_inuse(p.downstream_buffer));
  CHECK(audio_fork_ws_start(&p, 12345, 1) == AF_PCM_FORMAT);
  CHECK(!switch_atomic_read(&p.downstream_accept_audio));
  CHECK(!audio_fork_ws_start(&p, 8000, 1));
  char *large = calloc(1, 2U * 1024U * 1024U + 4);
  CHECK(large);
  CHECK(audio_fork_ws_produce(&p, large, 2U * 1024U * 1024U + 4) == AF_PCM_SINK);
  CHECK(!switch_buffer_inuse(p.downstream_buffer));
  free(large);
  af_pcm_destroy(&p.downstream_pcm);
  switch_buffer_destroy(&p.downstream_buffer);
  puts("WS: early audio, short EOF, padding, format lock/conflict, stale close, partial/overflow: passed");
}

static void http_tests(switch_memory_pool_t *pool) {
  audio_fork_http_ctx_t ctx = {0};
  ctx.channels = 1;
  ctx.samplerate = 8000;
  ctx.watchdog_ms = 200;
  CHECK(switch_mutex_init(&ctx.audio_mutex, SWITCH_MUTEX_NESTED, pool) == SWITCH_STATUS_SUCCESS);
  CHECK(switch_thread_cond_create(&ctx.audio_cond, pool) == SWITCH_STATUS_SUCCESS);
  CHECK(switch_buffer_create_dynamic(&ctx.audio_buffer, 4096, 16384, MAX_AUDIO_FORK_BUFFER_BYTES) == SWITCH_STATUS_SUCCESS);
  switch_file_handle_t handle = {0};
  handle.private_info = &ctx;
  int16_t input[37], output[16384];
  for (size_t i = 0; i < 37; ++i) input[i] = 2000;
  switch_buffer_write(ctx.audio_buffer, input, sizeof(input));
  size_t len = 16384;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS && len == 160);
  for (size_t i = 0; i < 160; ++i) CHECK(output[i] == (i < 37 ? 2000 : 0));
  CHECK(ctx.underrun_samples == 123);
  len = 160;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS);
  CHECK(!cond_waits); /* 包含空缓冲与部分帧，读端不得等待生产者。 */
  ctx.watchdog.since_us = af_monotonic_us() - 201000;
  len = 160;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);
  CHECK(switch_atomic_read(&ctx.abort_requested));
  switch_atomic_set(&ctx.abort_requested, 0);
  switch_atomic_set(&ctx.err, 0);
  switch_atomic_set(&ctx.eof, 1);
  switch_buffer_write(ctx.audio_buffer, input, sizeof(input));
  len = 160;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_SUCCESS && len == 37);
  len = 160;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);
  switch_atomic_set(&ctx.err, 1);
  switch_buffer_write(ctx.audio_buffer, input, sizeof(input));
  len = 160;
  CHECK(audio_fork_http_file_read(&handle, output, &len) == SWITCH_STATUS_FALSE && !len);
  switch_buffer_destroy(&ctx.audio_buffer);
  CHECK(!cond_waits);
  puts("HTTP: nonblocking empty/partial reads, EOF tail, watchdog and error exit: passed");
}

int main(void) {
  switch_memory_pool_t *pool = NULL;
  CHECK(!fspr_initialize());
  CHECK(!fspr_pool_create_ex(&pool, NULL, NULL, NULL));
  ws_tests(pool);
  http_tests(pool);
  fspr_pool_destroy(pool);
  fspr_terminate();
  return 0;
}
