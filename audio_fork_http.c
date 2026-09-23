#include "audio_fork_http.h"
#include <sys/socket.h>

/* ========================================================================= */
/* libcurl 回调函数                                                          */
/* ========================================================================= */

static int audio_fork_http_sockopt_cb(void *clientp, curl_socket_t curlfd, curlsocktype purpose) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)clientp;
  (void)purpose;
  if (ctx) {
    switch_mutex_lock(ctx->audio_mutex);
    ctx->curlfd = curlfd;
    switch_mutex_unlock(ctx->audio_mutex);
  }
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
  if (!ctx || ctx->abort_requested) {
    return 1; /* 非 0 值指示 libcurl 立即中断传输 (CURLE_ABORTED_BY_CALLBACK) */
  }
  return 0;
}

static size_t audio_fork_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)userdata;
  size_t total_bytes = size * nmemb;
  if (!ctx || ctx->abort_requested) {
    return 0; /* 返回 0 促使 curl 立即以写入错误中断 */
  }
  if (total_bytes == 0) {
    return 0;
  }

  switch_mutex_lock(ctx->audio_mutex);
  if (ctx->audio_buffer) {
    switch_size_t inuse = switch_buffer_inuse(ctx->audio_buffer);
    if (inuse + total_bytes > MAX_AUDIO_FORK_BUFFER_BYTES) {
      size_t frame_bytes = sizeof(int16_t) * ctx->channels;
      size_t overflow = (inuse + total_bytes) - MAX_AUDIO_FORK_BUFFER_BYTES;
      if (frame_bytes > 0 && (overflow % frame_bytes) != 0) {
        overflow += (frame_bytes - (overflow % frame_bytes));
      }
      switch_buffer_toss(ctx->audio_buffer, overflow);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
        "[audio_fork] HTTP 流缓冲超限 (50s)，采样对齐丢弃最旧 %zu 字节\n", overflow);
    }
    switch_buffer_write(ctx->audio_buffer, ptr, total_bytes);
  }
  switch_mutex_unlock(ctx->audio_mutex);

  return total_bytes;
}

static void *SWITCH_THREAD_FUNC audio_fork_http_thread(switch_thread_t *thread, void *obj) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)obj;
  switch_CURL *curl = NULL;
  switch_CURLcode res;
  char errbuf[CURL_ERROR_SIZE] = "";

  (void)thread;
  if (!ctx) return NULL;

  curl = switch_curl_easy_init();
  if (!curl) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] 初始化 curl 句柄失败\n");
    ctx->err = 1;
    ctx->eof = 1;
    return NULL;
  }

  switch_curl_easy_setopt(curl, CURLOPT_URL, ctx->stream_url);
  switch_curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  switch_curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  switch_curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  switch_curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
  switch_curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  switch_curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  switch_curl_easy_setopt(curl, CURLOPT_USERAGENT, "FreeSWITCH(mod_audio_fork)/1.0");

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

  res = switch_curl_easy_perform(curl);

  /* 获取 HTTP 响应状态码 */
  switch_curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &ctx->http_response_code);

  switch_mutex_lock(ctx->audio_mutex);
  ctx->curlfd = -1;
  switch_mutex_unlock(ctx->audio_mutex);

  if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK && res != CURLE_WRITE_ERROR) {
    if (!ctx->abort_requested) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
        "[audio_fork] CURL 请求异常 res=[%d] (%s) http_code=[%ld]: %s [%s]\n",
        res, switch_curl_easy_strerror(res), ctx->http_response_code, errbuf, ctx->stream_url);
      ctx->err = 1;
    }
  } else if (ctx->http_response_code >= 400) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
      "[audio_fork] HTTP 响应错误状态码: %ld [%s]\n", ctx->http_response_code, ctx->stream_url);
    ctx->err = 1;
  }

  switch_curl_easy_cleanup(curl);

  ctx->eof = 1;
  return NULL;
}

/* ========================================================================= */
/* SWITCH_FILE_INTERFACE 接口函数实现                                         */
/* ========================================================================= */

switch_status_t audio_fork_http_file_open(switch_file_handle_t *handle, const char *path) {
  if (!handle || !path) return SWITCH_STATUS_FALSE;

  if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork] 不支持写入模式\n");
    return SWITCH_STATUS_NOTIMPL;
  }

  const char *p = path;
  if (!strncasecmp(p, "audio_fork://", 13)) {
    p += 13;
  }

  char *stream_url = NULL;
  if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
    stream_url = switch_core_strdup(handle->memory_pool, p);
  } else {
    stream_url = switch_core_sprintf(handle->memory_pool, "http://%s", p);
  }

  uint32_t samplerate = 16000;
  uint32_t channels = 1;
  uint32_t watchdog_ms = 3000;
  uint32_t prebuffer_ms = AUDIO_FORK_PREBUFFER_DEFAULT_MS;

  char *query = strchr(stream_url, '?');
  if (query) {
    const char *v;
    if ((v = switch_stristr("rate=", query)) || (v = switch_stristr("sampling=", query))) {
      uint32_t r = (uint32_t)atoi(v + (v[0] == 'r' ? 5 : 9));
      if (r >= 8000 && r <= 48000) {
        samplerate = r;
      }
    }
    if ((v = switch_stristr("channels=", query))) {
      uint32_t c = (uint32_t)atoi(v + 9);
      if (c == 1 || c == 2) {
        channels = c;
      }
    }
    if ((v = switch_stristr("watchdog=", query))) {
      watchdog_ms = (uint32_t)atoi(v + 9);
    }
    if ((v = switch_stristr("prebuffer=", query))) {
      uint32_t pb = (uint32_t)atoi(v + 10);
      if (pb > 0 && pb <= 2000) {
        prebuffer_ms = pb;
      }
    }
  }

  uint32_t prebuffer_bytes = (samplerate * channels * (uint32_t)sizeof(int16_t) * prebuffer_ms) / 1000;
  if (prebuffer_bytes < 320) {
    prebuffer_bytes = 320;
  }

  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(audio_fork_http_ctx_t));
  if (!ctx) return SWITCH_STATUS_MEMERR;
  memset(ctx, 0, sizeof(*ctx));

  ctx->driver_type = AUDIO_FORK_DRIVER_HTTP;
  ctx->pool = handle->memory_pool;
  ctx->stream_url = stream_url;
  ctx->samplerate = samplerate;
  ctx->channels = channels;
  ctx->curlfd = -1;
  ctx->prebuffer_bytes = prebuffer_bytes;
  ctx->max_silence_frames = (watchdog_ms == 0) ? 0 : (watchdog_ms / 20);

  if (switch_mutex_init(&ctx->audio_mutex, SWITCH_MUTEX_NESTED, ctx->pool) != SWITCH_STATUS_SUCCESS) {
    return SWITCH_STATUS_GENERR;
  }

  if (switch_buffer_create_dynamic(&ctx->audio_buffer, 4096, 16384, MAX_AUDIO_FORK_BUFFER_BYTES) != SWITCH_STATUS_SUCCESS) {
    return SWITCH_STATUS_MEMERR;
  }

  /* 启动后台 curl 异步拉流线程 */
  switch_threadattr_t *thd_attr = NULL;
  switch_threadattr_create(&thd_attr, ctx->pool);
  switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
  switch_thread_create(&ctx->read_thread, thd_attr, audio_fork_http_thread, ctx, ctx->pool);

  /* 等待起播预缓冲或首包数据 */
  int sanity = 600; /* 600 * 5ms = 3000ms 最大首包等待 */
  while (--sanity > 0 && !ctx->abort_requested && !ctx->err) {
    if (ctx->eof) break;
    switch_mutex_lock(ctx->audio_mutex);
    if (switch_buffer_inuse(ctx->audio_buffer) >= ctx->prebuffer_bytes) {
      switch_mutex_unlock(ctx->audio_mutex);
      break;
    }
    switch_mutex_unlock(ctx->audio_mutex);
    switch_yield(5000); /* 5ms */
  }

  switch_mutex_lock(ctx->audio_mutex);
  size_t initial_inuse = ctx->audio_buffer ? switch_buffer_inuse(ctx->audio_buffer) : 0;
  switch_mutex_unlock(ctx->audio_mutex);

  if (ctx->err || (sanity <= 0 && initial_inuse == 0)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "[audio_fork] 打开 HTTP 流失败: %s (http_code=%ld, err=%d, inuse=%zu)\n",
      ctx->stream_url, ctx->http_response_code, ctx->err, initial_inuse);

    ctx->abort_requested = 1;
    switch_mutex_lock(ctx->audio_mutex);
    if (ctx->curlfd > -1) {
      shutdown(ctx->curlfd, SHUT_RDWR);
      ctx->curlfd = -1;
    }
    switch_mutex_unlock(ctx->audio_mutex);

    if (ctx->read_thread) {
      switch_status_t st;
      switch_thread_join(&st, ctx->read_thread);
      ctx->read_thread = NULL;
    }

    switch_mutex_lock(ctx->audio_mutex);
    if (ctx->audio_buffer) {
      switch_buffer_destroy(&ctx->audio_buffer);
    }
    switch_mutex_unlock(ctx->audio_mutex);

    return SWITCH_STATUS_GENERR;
  }

  handle->private_info = ctx;
  handle->samplerate = ctx->samplerate;
  handle->channels = ctx->channels;
  handle->real_channels = ctx->channels;
  handle->format = 0;
  handle->sections = 0;
  handle->seekable = 0;
  handle->speed = 0;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
    "[audio_fork] HTTP 流成功打开: %s (rate=%u, channels=%u, prebuf=%u bytes, watchdog=%u ms)\n",
    ctx->stream_url, ctx->samplerate, ctx->channels, ctx->prebuffer_bytes, watchdog_ms);

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_http_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)handle->private_info;
  if (!ctx || !data || !len) return SWITCH_STATUS_FALSE;

  size_t frame_bytes = sizeof(int16_t) * ctx->channels;
  size_t bytes_needed = *len * frame_bytes;
  size_t bytes_read = 0;

  if (ctx->abort_requested) {
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  switch_mutex_lock(ctx->audio_mutex);
  size_t inuse = ctx->audio_buffer ? switch_buffer_inuse(ctx->audio_buffer) : 0;

  /* 流结束或出错且缓冲已完全排空时，立即优雅退出，绝不注入虚假静音帧延误通道 */
  if (inuse == 0 && (ctx->eof || ctx->err)) {
    switch_mutex_unlock(ctx->audio_mutex);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  if (inuse > 0 && ctx->audio_buffer) {
    /* 强制以整采样为边界读取：即使 buffer 中有零碎奇数字节，也仅读取整采样倍数，
       剩余不足 1 个采样的残余字节保留在 buffer 中与后续流入数据拼接，杜绝采样错位 (Scratch Noise) */
    size_t safe_inuse = inuse - (inuse % frame_bytes);
    size_t to_read = (bytes_needed < safe_inuse) ? bytes_needed : safe_inuse;
    if (to_read > 0) {
      bytes_read = switch_buffer_read(ctx->audio_buffer, data, to_read);
      bytes_read -= (bytes_read % frame_bytes);
    }
  }
  switch_mutex_unlock(ctx->audio_mutex);

  /* 缓冲读取量不足期望量 bytes_needed 的处理 */
  if (bytes_read < bytes_needed) {
    /* 若底层流已结束 (EOF) 或发生错误，说明音频已全部读出，直接返回实际读取的采样数，严禁补 0 填充静音 */
    if (ctx->eof || ctx->err) {
      if (bytes_read == 0) {
        *len = 0;
        return SWITCH_STATUS_FALSE;
      }
      *len = bytes_read / frame_bytes;
      handle->sample_count += *len;
      return SWITCH_STATUS_SUCCESS;
    }

    /* 底层流尚未结束（网络流中途欠载抖动）：补静音维持 20ms RTP 时钟并启动看门狗 */
    memset((char *)data + bytes_read, 0, bytes_needed - bytes_read);

    ctx->silence_frames++;
    if (ctx->max_silence_frames > 0 && ctx->silence_frames > ctx->max_silence_frames) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
        "[audio_fork] 静音看门狗超时 (%u ms)，中止播放 %s\n",
        ctx->max_silence_frames * 20, ctx->stream_url);
      *len = 0;
      return SWITCH_STATUS_FALSE;
    }

    *len = bytes_needed / frame_bytes;
    handle->sample_count += *len;
    return SWITCH_STATUS_SUCCESS;
  }

  /* 读满了完整期望数据 */
  ctx->silence_frames = 0;
  *len = bytes_needed / frame_bytes;
  handle->sample_count += *len;
  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_http_file_close(switch_file_handle_t *handle) {
  audio_fork_http_ctx_t *ctx = (audio_fork_http_ctx_t *)handle->private_info;
  if (!ctx) return SWITCH_STATUS_SUCCESS;

  handle->private_info = NULL;
  ctx->abort_requested = 1;

  /* 1. 立即关闭底层 socket，触发 curl 读阻塞瞬间解除 */
  switch_mutex_lock(ctx->audio_mutex);
  if (ctx->curlfd > -1) {
    shutdown(ctx->curlfd, SHUT_RDWR);
    ctx->curlfd = -1;
  }
  switch_mutex_unlock(ctx->audio_mutex);

  /* 2. 安全等待后台拉流线程退出 */
  if (ctx->read_thread) {
    switch_status_t st;
    switch_thread_join(&st, ctx->read_thread);
    ctx->read_thread = NULL;
  }

  /* 3. 销毁缓冲区 */
  switch_mutex_lock(ctx->audio_mutex);
  if (ctx->audio_buffer) {
    switch_buffer_destroy(&ctx->audio_buffer);
  }
  switch_mutex_unlock(ctx->audio_mutex);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
    "[audio_fork] HTTP 流播放优雅关闭完成: %s\n", ctx->stream_url);

  return SWITCH_STATUS_SUCCESS;
}
