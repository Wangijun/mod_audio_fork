/**
 * @file audio_fork_http.c
 * @brief HTTP/HTTPS Chunked 异步拉流下行虚拟文件驱动实现
 *
 * 核心机制:
 * 1. 异步多线程拉流:
 *    - audio_fork_http_thread 在后台使用 switch_curl (libcurl) 执行 HTTP GET 请求;
 *    - audio_fork_http_write_cb 回调在收到 chunked 裸 PCM 时写入 audio_buffer 环形缓冲;
 * 2. 线程同步与背压门控:
 *    - 缓冲区满时, 写入线程在 audio_cond 上等待最多 250ms (AUDIO_FORK_WRITE_WAIT_US);
 *    - 读取线程仅消费输出率 PCM；欠载立即补零，不等待网络数据;
 * 3. 即时打断 (Barge-In / Hangup):
 *    - 记录底层 socket fd (curlfd), 在需要退出时调用 shutdown(curlfd, SHUT_RDWR)
 *      强行打断阻塞在内核套接字读取上的 libcurl, 确保即刻退出并安全 join 线程;
 * 4. 预缓冲与欠载处理:
 *    - 起播检测循环最多等待 3 秒 (sanity = 600 * 5ms), 达到目标预缓冲字节或 500ms 降级门限后开闸;
 *    - 播放中若缓冲读空, 连续欠载补静音帧, 并在超过看门狗上限时中止.
 */

#include "audio_fork_http.h"
#include <sys/socket.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/* 最大看门狗超时时间 (毫秒) */
#define AUDIO_FORK_MAX_WATCHDOG_MS 600000U
/* HTTP 写入环形缓冲区时的最大背压等待超时 (微秒 = 250ms) */
#define AUDIO_FORK_WRITE_WAIT_US   250000U

/**
 * @brief 解析 URL 查询参数中的无符号整型数值
 */
static int parse_query_uint(const char *query, const char *name, uint32_t max,
                            uint32_t *value, int *present) {
  size_t name_len = strlen(name);
  const char *cursor = query;
  if (present) *present = 0;
  while (cursor && *cursor) {
    const char *amp = strchr(cursor, '&');
    size_t token_len = amp ? (size_t)(amp - cursor) : strlen(cursor);
    if (token_len >= name_len + 1 && !strncasecmp(cursor, name, name_len) && cursor[name_len] == '=') {
      char number[32];
      size_t number_len = token_len - name_len - 1;
      if (number_len == 0 || number_len >= sizeof(number)) return -1;
      memcpy(number, cursor + name_len + 1, number_len);
      number[number_len] = '\0';
      if (number[0] == '-' || number[0] == '+') return -1;
      for (size_t i = 0; i < number_len; ++i) {
        if (number[i] < '0' || number[i] > '9') return -1;
      }
      errno = 0;
      char *end = NULL;
      unsigned long parsed = strtoul(number, &end, 10);
      if (errno == ERANGE || end == number || *end != '\0' || parsed > max) return -1;
      if (value) *value = (uint32_t)parsed;
      if (present) *present = 1;
      return 1;
    }
    if (!amp) break;
    cursor = amp + 1;
  }
  return 0;
}

/**
 * @brief 检查查询字符串中是否包含已被废弃的 prebuffer 参数
 */
static int query_has_prebuffer(const char *query) {
  const char *cursor = query;
  while (cursor && *cursor) {
    const char *amp = strchr(cursor, '&');
    size_t len = amp ? (size_t)(amp - cursor) : strlen(cursor);
    if (len >= 9 && !strncasecmp(cursor, "prebuffer", 9) &&
        (len == 9 || cursor[9] == '=')) return 1;
    cursor = amp ? amp + 1 : NULL;
  }
  return 0;
}

/**
 * @brief 计算起播预缓冲所需的字节数
 * 公式: rate * channels * 2 * milliseconds / 1000
 */
static int calculate_prebuffer_bytes(uint32_t rate, uint32_t channels,
                                     uint32_t milliseconds, uint32_t *result) {
  uint64_t bytes;
  if (!result || rate < 8000 || rate > 48000 || channels < 1 || channels > 2 ||
      milliseconds < 1 || milliseconds > 5000) return 0;
  bytes = (uint64_t)rate * channels * sizeof(int16_t) * milliseconds / 1000U;
  if (bytes == 0 || bytes > UINT32_MAX) return 0;
  *result = (uint32_t)bytes;
  return 1;
}

/**
 * @brief 设置 HTTP 错误与中止标记
 */
static void set_http_error(audio_fork_http_ctx_t *ctx) {
  if (!ctx) return;
  switch_atomic_set(&ctx->err, 1);
  switch_atomic_set(&ctx->abort_requested, 1);
}

/**
 * @brief 检查拉流是否已请求中止
 */
static int http_is_aborted(audio_fork_http_ctx_t *ctx) {
  return !ctx || switch_atomic_read(&ctx->abort_requested) != 0;
}

/**
 * @brief 停止后台拉流线程并清理所有内部动态资源 (套接字/条件变量/环形缓冲)
 */
static void audio_fork_http_cleanup_resources(audio_fork_http_ctx_t *ctx) {
  if (!ctx) return;
  switch_atomic_set(&ctx->abort_requested, 1);
  if (ctx->audio_mutex) {
    switch_mutex_lock(ctx->audio_mutex);
    if (ctx->curlfd > -1) {
      shutdown(ctx->curlfd, SHUT_RDWR);
      ctx->curlfd = -1;
    }
    if (ctx->audio_cond) switch_thread_cond_broadcast(ctx->audio_cond);
    switch_mutex_unlock(ctx->audio_mutex);
  }
  if (ctx->read_thread) {
    switch_status_t st;
    switch_thread_join(&st, ctx->read_thread);
    ctx->read_thread = NULL;
  }
  if (ctx->audio_mutex) {
    switch_mutex_lock(ctx->audio_mutex);
    if (ctx->audio_buffer) switch_buffer_destroy(&ctx->audio_buffer);
    if (ctx->audio_cond) switch_thread_cond_broadcast(ctx->audio_cond);
    switch_mutex_unlock(ctx->audio_mutex);
  }
  af_pcm_destroy(&ctx->pcm);
}

/**
 * @brief libcurl 套接字创建回调: 记录底层 socket fd 以支持即时 shutdown 解阻塞
 */
static int audio_fork_http_sockopt_cb(void *clientp, curl_socket_t curlfd, curlsocktype purpose) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)clientp;
  (void)purpose;
  if (ctx && ctx->audio_mutex) {
    switch_mutex_lock(ctx->audio_mutex);
    ctx->curlfd = curlfd;
    switch_mutex_unlock(ctx->audio_mutex);
  }
  return 0;
}

/* curl 关闭 fd 前撤销记录，避免 close/abort 竞态误 shutdown 被系统复用的描述符。 */
static int audio_fork_http_closesocket_cb(void *clientp, curl_socket_t fd) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)clientp;
  switch_mutex_lock(ctx->audio_mutex);
  if (ctx->curlfd == fd) ctx->curlfd = -1;
  close(fd);
  switch_mutex_unlock(ctx->audio_mutex);
  return 0;
}

#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072000)
static int audio_fork_http_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
#else
static int audio_fork_http_progress_cb(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
#endif
{
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)clientp;
  (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  /* 进度回调返回非 0 即可立即中止 curl 传输 */
  return http_is_aborted(ctx) ? 1 : 0;
}

/**
 * @brief libcurl 接收数据写入回调: 将接收到的裸 PCM chunk 写入环形缓冲区
 */
static int http_pcm_sink(void *userdata, const int16_t *pcm, size_t total_bytes) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)userdata;
  uint64_t deadline = af_monotonic_us() + AUDIO_FORK_WRITE_WAIT_US;
  switch_mutex_lock(ctx->audio_mutex);
  while (!http_is_aborted(ctx) && ctx->audio_buffer) {
    size_t inuse = switch_buffer_inuse(ctx->audio_buffer);
    if (inuse <= MAX_AUDIO_FORK_BUFFER_BYTES && total_bytes <= MAX_AUDIO_FORK_BUFFER_BYTES - inuse) {
      switch_buffer_write(ctx->audio_buffer, pcm, total_bytes);
      int ok = switch_buffer_inuse(ctx->audio_buffer) == inuse + total_bytes;
      if (ok) ++ctx->audio_serial;
      switch_mutex_unlock(ctx->audio_mutex);
      return ok ? 0 : -1;
    }
    if (af_monotonic_us() >= deadline) break;
    switch_thread_cond_timedwait(ctx->audio_cond, ctx->audio_mutex, 5000);
  }
  switch_mutex_unlock(ctx->audio_mutex);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] HTTP PCM 缓冲溢出或生产者背压超时\n");
  return -1;
}

static size_t audio_fork_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)userdata;
  if (!ctx || !ptr || !size || nmemb > SIZE_MAX / size || http_is_aborted(ctx)) return 0;
  size_t bytes = size * nmemb;
  int error = af_pcm_write(ctx->pcm, ptr, bytes, http_pcm_sink, ctx);
  if (error) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] HTTP 下行转换失败: %s\n", af_pcm_error(error));
    set_http_error(ctx);
    return 0;
  }
  return bytes;
}

/**
 * @brief 后台 curl 异步拉流工作线程函数
 */
static void *SWITCH_THREAD_FUNC audio_fork_http_thread(switch_thread_t *thread, void *obj) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)obj;
  switch_CURL *curl = NULL;
  switch_CURLcode res;
  long response_code = 0;
  char errbuf[CURL_ERROR_SIZE] = "";
  (void)thread;
  if (!ctx) return NULL;

  curl = switch_curl_easy_init();
  if (!curl) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] 初始化 curl 句柄失败\n");
    set_http_error(ctx);
    switch_atomic_set(&ctx->eof, 1);
    return NULL;
  }
  switch_curl_easy_setopt(curl, CURLOPT_URL, ctx->stream_url);
  switch_curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  switch_curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  switch_curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
  switch_curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  switch_curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  switch_curl_easy_setopt(curl, CURLOPT_USERAGENT, "FreeSWITCH(mod_audio_fork)/1.0");
  switch_curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audio_fork_http_write_cb);
  switch_curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)ctx);
#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= 0x072000)
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, audio_fork_http_progress_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#else
  curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, audio_fork_http_progress_cb);
  curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void *)ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#endif
  curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, audio_fork_http_sockopt_cb);
  curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, (void *)ctx);
  curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, audio_fork_http_closesocket_cb);
  curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, (void *)ctx);

  res = switch_curl_easy_perform(curl);
  switch_curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  switch_mutex_lock(ctx->audio_mutex);
  ctx->http_response_code = response_code;
  ctx->curlfd = -1;
  if (ctx->audio_cond) switch_thread_cond_broadcast(ctx->audio_cond);
  switch_mutex_unlock(ctx->audio_mutex);
  if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK && res != CURLE_WRITE_ERROR && !http_is_aborted(ctx)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
      "[audio_fork] CURL 请求异常 res=[%d] (%s) http_code=[%ld]: %s [%s]\n",
      res, switch_curl_easy_strerror(res), response_code, errbuf, ctx->stream_url);
    set_http_error(ctx);
  } else if (response_code >= 400 && !http_is_aborted(ctx)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
      "[audio_fork] HTTP 响应错误状态码: %ld [%s]\n", response_code, ctx->stream_url);
    set_http_error(ctx);
  }
  if (res == CURLE_OK && response_code < 400 && !http_is_aborted(ctx)) {
    int error = af_pcm_finish(ctx->pcm, http_pcm_sink, ctx);
    if (error) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] HTTP EOF 转换失败: %s\n", af_pcm_error(error));
      set_http_error(ctx);
    }
  }
  switch_curl_easy_cleanup(curl);
  switch_mutex_lock(ctx->audio_mutex);
  switch_atomic_set(&ctx->eof, 1);
  if (ctx->audio_cond) switch_thread_cond_broadcast(ctx->audio_cond);
  switch_mutex_unlock(ctx->audio_mutex);
  return NULL;
}

switch_status_t audio_fork_http_file_open(switch_file_handle_t *handle, const char *path) {
  if (!handle || !path || switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) return SWITCH_STATUS_FALSE;
  size_t input_len = strnlen(path, MAX_WS_URI_LEN + 1);
  if (input_len == 0 || input_len > MAX_WS_URI_LEN) return SWITCH_STATUS_FALSE;
  for (size_t i = 0; i < input_len; ++i) {
    unsigned char c = (unsigned char)path[i];
    if (c < 0x20 || c == 0x7f) return SWITCH_STATUS_FALSE;
  }
  const char *p = path;
  if (!strncasecmp(p, "audio_fork://", 13)) p += 13;
  char *stream_url = NULL;
  if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
    stream_url = switch_core_strdup(handle->memory_pool, p);
  } else {
    stream_url = switch_core_sprintf(handle->memory_pool, "http://%s", p);
  }
  if (!stream_url) return SWITCH_STATUS_MEMERR;

  /* file_open 入口 samplerate 是核心请求的播放率，URL rate 是源率。 */
  uint32_t output_rate = handle->samplerate;
  uint32_t samplerate = 16000, channels = 1, watchdog_ms = 3000;
  uint32_t prebuffer_ms = audio_fork_http_prebuffer_ms;
  char *query = strchr(stream_url, '?');
  int present = 0;
  int sampling_present = 0;
  uint32_t sampling_value = 0;

  if (query) {
    if (parse_query_uint(query + 1, "rate", 48000, &samplerate, &present) < 0) return SWITCH_STATUS_FALSE;
    if (parse_query_uint(query + 1, "sampling", 48000, &sampling_value, &sampling_present) < 0) return SWITCH_STATUS_FALSE;
    if (sampling_present && present && sampling_value != samplerate) return SWITCH_STATUS_FALSE;
    if (sampling_present && !present) { samplerate = sampling_value; present = 1; }
    if (present && (samplerate < 8000 || samplerate > 48000 || samplerate % 8000 != 0)) return SWITCH_STATUS_FALSE;
    present = 0;
    if (parse_query_uint(query + 1, "channels", 2, &channels, &present) < 0) return SWITCH_STATUS_FALSE;
    if (present && (channels != 1 && channels != 2)) return SWITCH_STATUS_FALSE;
    present = 0;
    if (parse_query_uint(query + 1, "watchdog", AUDIO_FORK_MAX_WATCHDOG_MS, &watchdog_ms, &present) < 0) return SWITCH_STATUS_FALSE;
    if (query_has_prebuffer(query + 1)) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                        "mod_audio_fork: HTTP URL 的 prebuffer 参数已被模组忽略, 使用 http-prebuffer-ms=%u ms\n",
                        prebuffer_ms);
    }
  }

  uint32_t prebuffer_bytes;
  if (!calculate_prebuffer_bytes(output_rate, channels, prebuffer_ms, &prebuffer_bytes)) return SWITCH_STATUS_FALSE;

  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(*ctx));
  if (!ctx) return SWITCH_STATUS_MEMERR;
  memset(ctx, 0, sizeof(*ctx));
  ctx->driver_type = AUDIO_FORK_DRIVER_HTTP;
  ctx->pool = handle->memory_pool;
  ctx->stream_url = stream_url;
  ctx->source_rate = samplerate;
  ctx->samplerate = output_rate;
  ctx->pcm = af_pcm_create(samplerate, output_rate, channels, SWITCH_RESAMPLE_QUALITY);
  if (!ctx->pcm) return SWITCH_STATUS_MEMERR;
  ctx->channels = channels;
  ctx->curlfd = -1;
  ctx->prebuffer_bytes = prebuffer_bytes;
  ctx->watchdog_ms = watchdog_ms;
  switch_atomic_set(&ctx->abort_requested, 0);
  switch_atomic_set(&ctx->eof, 0);
  switch_atomic_set(&ctx->err, 0);

  if (switch_mutex_init(&ctx->audio_mutex, SWITCH_MUTEX_NESTED, ctx->pool) != SWITCH_STATUS_SUCCESS ||
      switch_thread_cond_create(&ctx->audio_cond, ctx->pool) != SWITCH_STATUS_SUCCESS ||
      switch_buffer_create_dynamic(&ctx->audio_buffer, 4096, 16384, MAX_AUDIO_FORK_BUFFER_BYTES) != SWITCH_STATUS_SUCCESS) {
    audio_fork_http_cleanup_resources(ctx);
    return SWITCH_STATUS_MEMERR;
  }

  switch_threadattr_t *thd_attr = NULL;
  if (switch_threadattr_create(&thd_attr, ctx->pool) != SWITCH_STATUS_SUCCESS ||
      switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE) != SWITCH_STATUS_SUCCESS ||
      switch_thread_create(&ctx->read_thread, thd_attr, audio_fork_http_thread, ctx, ctx->pool) != SWITCH_STATUS_SUCCESS) {
    set_http_error(ctx);
    audio_fork_http_cleanup_resources(ctx);
    return SWITCH_STATUS_GENERR;
  }

  /* 起播预缓冲检测循环: 每 5ms 轮询一次, 最多等待 3000ms (600 次迭代) */
  int sanity = 600;
  uint32_t degradation_bytes = (uint32_t)(((uint64_t)output_rate * channels * sizeof(int16_t) * 500U) / 1000U);
  while (--sanity > 0 && !switch_atomic_read(&ctx->abort_requested) && !switch_atomic_read(&ctx->err)) {
    switch_mutex_lock(ctx->audio_mutex);
    size_t inuse = ctx->audio_buffer ? switch_buffer_inuse(ctx->audio_buffer) : 0;
    int eof = (int)switch_atomic_read(&ctx->eof);
    switch_mutex_unlock(ctx->audio_mutex);
    if (eof) break;
    /* 达到预缓冲门限或超过 1 秒 (sanity <= 400) 达到 500ms 降级门限时开闸 */
    if (inuse >= ctx->prebuffer_bytes || (sanity <= 400 && inuse >= degradation_bytes)) break;
    switch_yield(5000);
  }

  switch_mutex_lock(ctx->audio_mutex);
  size_t initial_inuse = ctx->audio_buffer ? switch_buffer_inuse(ctx->audio_buffer) : 0;
  if (ctx->audio_cond) switch_thread_cond_signal(ctx->audio_cond);
  switch_mutex_unlock(ctx->audio_mutex);

  if (switch_atomic_read(&ctx->err) ||
      (switch_atomic_read(&ctx->eof) && initial_inuse == 0) ||
      (sanity <= 0 && initial_inuse == 0)) {
    set_http_error(ctx);
    audio_fork_http_cleanup_resources(ctx);
    return SWITCH_STATUS_GENERR;
  }

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                    "mod_audio_fork: HTTP 播放预缓冲配置 %u ms, 本次起播缓冲 %zu 字节(目标 %u 字节)%s\n",
                    prebuffer_ms, initial_inuse, ctx->prebuffer_bytes,
                    initial_inuse < ctx->prebuffer_bytes ? ", 慢流降级或流结束" : "");

  handle->private_info = ctx;
  handle->samplerate = output_rate;
  handle->channels = (uint8_t)channels;
  handle->real_channels = (uint8_t)channels;
  handle->format = 0;
  handle->sections = 0;
  handle->seekable = 0;
  handle->speed = 0;
  /* 模组底层已实现专有环形音频缓冲与起播预缓冲机制, 彻底禁用 FreeSWITCH 核心层冗余的 64KB 预缓冲,
   * 杜绝核心层在首帧一次性抽干 64KB (2048ms) 导致实时流式音频瞬间读空并下溢 (对齐 mod_local_stream 实现) */
  handle->pre_buffer_datalen = 0;
  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_http_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
  if (!handle || !handle->private_info || !data || !len || !*len) return SWITCH_STATUS_FALSE;
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)handle->private_info;
  size_t frame_bytes = sizeof(int16_t) * ctx->channels;
  if (!frame_bytes || *len > SIZE_MAX / frame_bytes) { *len = 0; return SWITCH_STATUS_FALSE; }
  size_t bytes = *len * frame_bytes, read = 0;
  switch_mutex_lock(ctx->audio_mutex);
  int eof = (int)switch_atomic_read(&ctx->eof);
  /* playback 会预取 16384 个采样；活跃流只交付一个时钟帧，避免预填未来数秒静音。 */
  if (!eof && *len > ctx->samplerate / 50U) {
    *len = ctx->samplerate / 50U;
    bytes = *len * frame_bytes;
  }
  int failed = switch_atomic_read(&ctx->err) || switch_atomic_read(&ctx->abort_requested);
  if (!failed && ctx->audio_buffer) {
    size_t inuse = switch_buffer_inuse(ctx->audio_buffer);
    read = switch_buffer_read(ctx->audio_buffer, data, inuse < bytes ? inuse : bytes);
  }
  int expired = af_watchdog_expired(&ctx->watchdog, ctx->audio_serial,
                                   read > 0, !eof && read < bytes, ctx->watchdog_ms);
  if (ctx->audio_cond) switch_thread_cond_signal(ctx->audio_cond);
  switch_mutex_unlock(ctx->audio_mutex);
  if (failed || expired) {
    if (expired) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[audio_fork] HTTP 连续欠载达到 %u ms\n", ctx->watchdog_ms);
      set_http_error(ctx);
    }
    *len = 0; return SWITCH_STATUS_FALSE;
  }
  if (!eof && read < bytes) {
    memset((char *)data + read, 0, bytes - read);
    ++ctx->underrun_frames;
    ctx->underrun_samples += (bytes - read) / frame_bytes;
    read = bytes;
  }
  *len = read / frame_bytes;
  handle->sample_count += *len;
  return *len ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_status_t audio_fork_http_file_close(switch_file_handle_t *handle) {
  if (!handle || !handle->private_info) return SWITCH_STATUS_SUCCESS;
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)handle->private_info;
  handle->private_info = NULL;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
    "[audio_fork] HTTP playback source=%u output=%u core_resampler=%d samples=%lu url=%s\n",
    ctx->source_rate, ctx->samplerate, handle->resampler != NULL, (unsigned long)handle->sample_count, ctx->stream_url);

  /* 输出播放期间的欠载统计日志 (便于排查弱网或 TTS 推流抖动) */
  if (ctx->underrun_frames) {
    uint32_t underrun_ms = ctx->samplerate ? (uint32_t)(ctx->underrun_samples * 1000ULL / ctx->samplerate) : (ctx->underrun_frames * 20U);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                      "mod_audio_fork: HTTP 播放期间缓冲读空 %u 次(共 %u ms), URL=%s\n",
                      ctx->underrun_frames, underrun_ms, ctx->stream_url);
  }

  /* 安全释放拉流线程与音频缓冲资源 */
  audio_fork_http_cleanup_resources(ctx);
  return SWITCH_STATUS_SUCCESS;
}
