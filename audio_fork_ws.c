/**
 * @file audio_fork_ws.c
 * @brief WebSocket 全双工下行内存桥虚拟文件驱动实现
 *
 * 核心设计:
 * 1. 内存桥接模型:
 *    - 读端 (FreeSWITCH 核心播放引擎): 调用 audio_fork_ws_file_read(), 从 downstream_buffer 读取 S16LE PCM;
 *    - 写端 (LWS 事件分发线程): 收到下行 BINARY 帧, 由 processIncomingBinary() 将 PCM 追加写入 downstream_buffer;
 * 2. 状态机与保护机制:
 *    - downstream_active: 标记当前是否有活跃的下行播放句柄;
 *    - downstream_generation: 代际计数器, speak_start 推进代际, 句柄如果 generation 不匹配则判定为旧播报并优雅退出;
 *    - downstream_pcm: 生产者处理分包对齐、重采样与 EOF 延迟排空;
 *    - downstream_interrupted: 即时打断标记, 收到 kill_audio 信令后置 1 并清空环形缓冲.
 */

#include "audio_fork_ws.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* 看门狗最大超时保护限制 (10 分钟) */
#define WS_MAX_WATCHDOG_MS                 600000

/**
 * @brief 解析 URL 查询字符串中的无符号整型参数
 * @param query 查询字符串 (形如 "rate=16000&channels=1")
 * @param name 目标参数名
 * @param max 参数允许的最大上限
 * @param[out] value 解析出的数值存储地址
 * @param[out] present 是否存在该参数的布尔标记
 * @return 1=成功解析并赋值, 0=未找到该参数, -1=参数格式非法或超出范围
 */
static int parse_query_uint(const char *query, const char *name, uint32_t max,
                            uint32_t *value, int *present) {
  const size_t name_len = strlen(name);
  const char *cursor = query;
  if (present) *present = 0;
  while (cursor && *cursor) {
    const char *token = cursor;
    const char *amp = strchr(token, '&');
    size_t token_len = amp ? (size_t)(amp - token) : strlen(token);
    if (token_len >= name_len + 1 && !strncasecmp(token, name, name_len) && token[name_len] == '=') {
      char number[32];
      size_t number_len = token_len - name_len - 1;
      if (number_len == 0 || number_len >= sizeof(number)) return -1;
      memcpy(number, token + name_len + 1, number_len);
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
 * @brief 根据采样率、声道数与毫秒数精确计算起播预缓冲所需的字节数
 * 公式: bytes = rate * channels * 2(bytes/sample) * ms / 1000
 */
static int calculate_prebuffer_bytes(uint32_t rate, uint32_t channels,
                                     uint32_t milliseconds, uint32_t *result) {
  uint64_t bytes;
  if (!result || rate < 8000 || rate > 64000 || channels < 1 || channels > 2 ||
      milliseconds < 1 || milliseconds > 5000) return 0;
  bytes = (uint64_t)rate * channels * sizeof(int16_t) * milliseconds / 1000U;
  if (bytes == 0 || bytes > UINT32_MAX) return 0;
  *result = (uint32_t)bytes;
  return 1;
}

/* 以下生产者操作都在同一把下行锁中完成，转换与缓冲格式原子切换。 */
static int ws_pcm_sink(void *user, const int16_t *pcm, size_t bytes) {
  private_t *p = (private_t *)user;
  size_t before = p->downstream_buffer ? switch_buffer_inuse(p->downstream_buffer) : 0;
  if (!p->downstream_buffer || before > 2U * 1024U * 1024U ||
      bytes > 2U * 1024U * 1024U - before) return -1;
  switch_buffer_write(p->downstream_buffer, pcm, bytes);
  if (switch_buffer_inuse(p->downstream_buffer) != before + bytes) return -1;
  ++p->downstream_audio_serial;
  return 0;
}

int audio_fork_ws_format_locked(private_t *p, uint32_t source,
                                uint32_t channels, uint32_t output) {
  if (output != p->downstream_output_rate || channels != p->downstream_channels ||
      source < 8000 || source > 64000 || source % 8000 ||
      (source != p->downstream_sample_rate &&
       (p->downstream_rate_declared || p->downstream_audio_started))) return AF_PCM_FORMAT;
  if (source != p->downstream_sample_rate) {
    int error = af_pcm_reset(p->downstream_pcm, source, output, channels);
    if (error) return error;
    p->downstream_sample_rate = source;
  }
  return AF_PCM_OK;
}

int audio_fork_ws_start(private_t *p, uint32_t rate, int declared) {
  int error;
  switch_mutex_lock(p->downstream_mutex);
  ++p->downstream_generation;
  p->downstream_current_handles = 0;
  p->downstream_audio_started = 0;
  p->downstream_rate_declared = declared;
  p->downstream_sample_rate = declared ? rate : p->downstream_default_rate;
  if (p->downstream_buffer) switch_buffer_zero(p->downstream_buffer);
  switch_atomic_set(&p->downstream_error, 0);
  switch_atomic_set(&p->downstream_eof, 0);
  error = (!p->downstream_sample_rate || p->downstream_sample_rate % 8000) ? AF_PCM_FORMAT :
    af_pcm_reset(p->downstream_pcm, p->downstream_sample_rate, p->downstream_output_rate, p->downstream_channels);
  switch_atomic_set(&p->downstream_interrupted, error ? 1 : 0);
  switch_atomic_set(&p->downstream_accept_audio, error ? 0 : 1);
  switch_mutex_unlock(p->downstream_mutex);
  return error;
}

int audio_fork_ws_produce(private_t *p, const void *data, size_t bytes) {
  int error = AF_PCM_OK;
  switch_mutex_lock(p->downstream_mutex);
  if (p->downstream_buffer && switch_atomic_read(&p->downstream_accept_audio) &&
      !switch_atomic_read(&p->downstream_interrupted) && !switch_atomic_read(&p->downstream_eof)) {
    if (bytes) {
      p->downstream_audio_started = 1;
      error = af_pcm_write(p->downstream_pcm, data, bytes, ws_pcm_sink, p);
    } else {
      error = af_pcm_finish(p->downstream_pcm, ws_pcm_sink, p);
      switch_atomic_set(&p->downstream_eof, 1);
      switch_atomic_set(&p->downstream_accept_audio, 0);
    }
    if (error) {
      switch_atomic_set(&p->downstream_error, 1);
      switch_atomic_set(&p->downstream_accept_audio, 0);
      switch_atomic_set(&p->downstream_interrupted, 1);
      switch_atomic_set(&p->downstream_eof, 1);
      switch_buffer_zero(p->downstream_buffer);
      af_pcm_reset(p->downstream_pcm, p->downstream_sample_rate, p->downstream_output_rate, p->downstream_channels);
    }
  }
  switch_mutex_unlock(p->downstream_mutex);
  return error;
}

switch_status_t audio_fork_ws_file_open(switch_file_handle_t *handle, const char *path) {
  if (!handle || !path) return SWITCH_STATUS_FALSE;
  /* 下行驱动仅支持播放 (READ), 不支持写入 (WRITE) */
  if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) return SWITCH_STATUS_NOTIMPL;

  const char *p = path;
  if (!strncasecmp(p, "audio_fork://", 13)) p += 13;
  size_t path_len = strnlen(p, MAX_PATH_LEN + MAX_BUG_LEN + 3);
  if (path_len == 0 || path_len > MAX_PATH_LEN + MAX_BUG_LEN + 1) return SWITCH_STATUS_FALSE;

  /* 解析路径中的 UUID、可选的 bugname 与查询参数 (?query) */
  char mypath[MAX_PATH_LEN + MAX_BUG_LEN + 2];
  memcpy(mypath, p, path_len);
  mypath[path_len] = '\0';
  char *query = strchr(mypath, '?');
  if (query) *query++ = '\0';

  char bugname[MAX_BUG_LEN + 1] = {0};
  char *bugname_part = strchr(mypath, ':');
  if (bugname_part) {
    *bugname_part++ = '\0';
    if (zstr(bugname_part) || strlen(bugname_part) > MAX_BUG_LEN) return SWITCH_STATUS_FALSE;
    switch_copy_string(bugname, bugname_part, sizeof(bugname));
  } else {
    switch_copy_string(bugname, MY_BUG_NAME, sizeof(bugname));
  }
  const char *uuid = mypath;
  if (zstr(uuid) || strlen(uuid) >= MAX_SESSION_ID) return SWITCH_STATUS_FALSE;

  /* 检索并锁定关联的 FreeSWITCH 核心会话 (获取 rwlock) */
  switch_core_session_t *lsession = switch_core_session_locate(uuid);
  if (!lsession) return SWITCH_STATUS_FALSE;
  switch_channel_t *channel = switch_core_session_get_channel(lsession);
  switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);
  if (!bug) {
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  private_t *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
  if (!tech_pvt || !tech_pvt->downstream_mutex) {
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(*ctx));
  if (!ctx) { switch_core_session_rwunlock(lsession); return SWITCH_STATUS_MEMERR; }
  memset(ctx, 0, sizeof(*ctx));
  if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
  if (switch_atomic_read(&tech_pvt->lifecycle_state) != AUDIO_FORK_LIFECYCLE_ACTIVE) {
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }

  uint32_t samplerate, channels, session_channels;
  uint32_t watchdog_ms = 3000, prebuffer_ms = audio_fork_ws_prebuffer_ms;
  uint64_t generation;
  int present = 0;
  int sampling_present = 0;
  uint32_t sampling_value = 0;
  switch_mutex_lock(tech_pvt->downstream_mutex);
  if (!tech_pvt->downstream_buffer || switch_atomic_read(&tech_pvt->downstream_error)) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  samplerate = tech_pvt->downstream_sample_rate ? tech_pvt->downstream_sample_rate : 16000;
  channels = tech_pvt->downstream_channels ? tech_pvt->downstream_channels : 1;
  session_channels = channels;

  /* 校验可选参数: rate 与 sampling 别名、channels 与 watchdog */
  if (query && parse_query_uint(query, "rate", 64000, &samplerate, &present) < 0) goto invalid_query;
  if (query && parse_query_uint(query, "sampling", 64000, &sampling_value, &sampling_present) < 0) goto invalid_query;
  if (sampling_present && present && sampling_value != samplerate) goto invalid_query;
  if (sampling_present && !present) { samplerate = sampling_value; present = 1; }
  if (present && (samplerate < 8000 || samplerate > 64000 || samplerate % 8000 != 0)) goto invalid_query;
  present = 0;
  if (query && parse_query_uint(query, "channels", 2, &channels, &present) < 0) goto invalid_query;
  if (present && ((channels != 1 && channels != 2) || channels != session_channels)) goto invalid_query;
  present = 0;
  if (query && parse_query_uint(query, "watchdog", WS_MAX_WATCHDOG_MS, &watchdog_ms, &present) < 0) goto invalid_query;
  if (query && query_has_prebuffer(query)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_audio_fork: WebSocket URL 的 prebuffer 参数已被模组忽略, 使用 ws-prebuffer-ms=%u ms\n",
                      prebuffer_ms);
  }

  if (audio_fork_ws_format_locked(tech_pvt, samplerate, channels, handle->samplerate)) goto invalid_query;
  samplerate = tech_pvt->downstream_output_rate;
  uint32_t prebuffer_bytes;
  if (!calculate_prebuffer_bytes(samplerate, channels, prebuffer_ms, &prebuffer_bytes)) goto invalid_query;
  ++tech_pvt->downstream_current_handles;

  /* speak_start 已经推进 generation; open 只记录当前代际, 不能再次推进,
     否则合法的 speak_start -> open 顺序会让句柄立即失效. */
  generation = tech_pvt->downstream_generation;
  switch_atomic_add(&tech_pvt->downstream_handles, 1);
  /* 保留 speak_start/binary 在 open 之前到达的 EOF, 残余字节和缓冲数据;
     新句的接收状态由 speak_start 原子地复位. */
  switch_atomic_set(&tech_pvt->downstream_active, 1);
  switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
  switch_atomic_set(&tech_pvt->downstream_accept_audio, 1);
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

  ctx->driver_type = AUDIO_FORK_DRIVER_WS;
  ctx->pool = handle->memory_pool;
  ctx->session = lsession;
  ctx->tech_pvt = tech_pvt;
  switch_copy_string(ctx->uuid, uuid, sizeof(ctx->uuid));
  switch_copy_string(ctx->bugname, bugname, sizeof(ctx->bugname));
  ctx->samplerate = samplerate;
  ctx->channels = channels;
  ctx->prebuffer_bytes = prebuffer_bytes;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                    "mod_audio_fork: WebSocket 播放预缓冲配置 %u ms, 本次目标 %u 字节\n",
                    prebuffer_ms, prebuffer_bytes);
  ctx->watchdog_ms = watchdog_ms;
  ctx->generation = generation;
  handle->samplerate = samplerate;
  handle->channels = (uint8_t)channels;
  handle->real_channels = (uint8_t)channels;
  handle->format = 0;
  handle->sections = 0;
  handle->seekable = 0;
  handle->speed = 0;
  handle->pos = 0;
  handle->private_info = ctx;
  /* 模组底层已实现专有环形音频缓冲与起播预缓冲机制, 彻底禁用 FreeSWITCH 核心层冗余的 64KB 预缓冲 */
  handle->pre_buffer_datalen = 0;
  return SWITCH_STATUS_SUCCESS;

invalid_query:
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
    "[audio_fork] WS format/query rejected: source=%u session_source=%u output=%u requested_output=%u channels=%u\n",
    samplerate, tech_pvt->downstream_sample_rate, tech_pvt->downstream_output_rate, handle->samplerate, channels);
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
  switch_core_session_rwunlock(lsession);
  return SWITCH_STATUS_FALSE;
}

switch_status_t audio_fork_ws_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
  if (!handle || !handle->private_info || !data || !len || !*len) return SWITCH_STATUS_FALSE;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;
  private_t *p = ctx->tech_pvt;
  size_t frame_bytes = sizeof(int16_t) * ctx->channels, bytes, read = 0;
  if (!frame_bytes || *len > SIZE_MAX / frame_bytes || !p || !p->downstream_mutex) {
    *len = 0; return SWITCH_STATUS_FALSE;
  }
  bytes = *len * frame_bytes;
  switch_mutex_lock(p->downstream_mutex);
  if (!p->downstream_buffer || ctx->generation != p->downstream_generation ||
      switch_atomic_read(&p->downstream_interrupted)) {
    switch_mutex_unlock(p->downstream_mutex);
    *len = 0; return SWITCH_STATUS_FALSE;
  }
  size_t inuse = switch_buffer_inuse(p->downstream_buffer);
  int eof = (int)switch_atomic_read(&p->downstream_eof);
  /* 限制活跃流预取到 20ms，欠载补零不得提前进入核心的数秒文件缓存。 */
  if (!eof && *len > ctx->samplerate / 50U) {
    *len = ctx->samplerate / 50U;
    bytes = *len * frame_bytes;
  }
  if (!ctx->prebuffered && (inuse >= ctx->prebuffer_bytes || eof)) ctx->prebuffered = 1;
  if (ctx->prebuffered) read = switch_buffer_read(p->downstream_buffer, data, inuse < bytes ? inuse : bytes);
  int expired = af_watchdog_expired(&ctx->watchdog, p->downstream_audio_serial,
                                   read > 0, !eof && read < bytes, ctx->watchdog_ms);
  switch_mutex_unlock(p->downstream_mutex);
  if (expired) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[audio_fork] WS 连续欠载达到 %u ms\n", ctx->watchdog_ms);
    *len = 0; return SWITCH_STATUS_FALSE;
  }
  if (!eof && read < bytes) {
    memset((char *)data + read, 0, bytes - read);
    read = bytes;
  }
  *len = read / frame_bytes;
  handle->sample_count += *len;
  return *len ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_status_t audio_fork_ws_file_close(switch_file_handle_t *handle) {
  if (!handle || !handle->private_info) return SWITCH_STATUS_SUCCESS;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
    "[audio_fork] WS playback output=%u core_resampler=%d samples=%lu uuid=%s\n",
    ctx->samplerate, handle->resampler != NULL, (unsigned long)handle->sample_count, ctx->uuid);
  if (ctx->tech_pvt && ctx->tech_pvt->downstream_mutex) {
    switch_mutex_lock(ctx->tech_pvt->downstream_mutex);
    unsigned int handles = switch_atomic_read(&ctx->tech_pvt->downstream_handles);
    if (handles > 0) --handles;
    switch_atomic_set(&ctx->tech_pvt->downstream_handles, handles);
    switch_atomic_set(&ctx->tech_pvt->downstream_active, handles > 0 ? 1U : 0U);
    if (ctx->generation == ctx->tech_pvt->downstream_generation) {
      if (ctx->tech_pvt->downstream_current_handles) --ctx->tech_pvt->downstream_current_handles;
      if (!ctx->tech_pvt->downstream_current_handles) {
        switch_atomic_set(&ctx->tech_pvt->downstream_interrupted, 1);
        switch_atomic_set(&ctx->tech_pvt->downstream_accept_audio, 0);
        switch_atomic_set(&ctx->tech_pvt->downstream_eof, 0);
        if (ctx->tech_pvt->downstream_buffer) switch_buffer_zero(ctx->tech_pvt->downstream_buffer);
        af_pcm_reset(ctx->tech_pvt->downstream_pcm, ctx->tech_pvt->downstream_sample_rate,
                     ctx->tech_pvt->downstream_output_rate, ctx->tech_pvt->downstream_channels);
      }
    }
    if ((switch_atomic_read(&ctx->tech_pvt->cleanup_started) ||
         switch_atomic_read(&ctx->tech_pvt->lifecycle_state) != AUDIO_FORK_LIFECYCLE_ACTIVE) &&
        switch_atomic_read(&ctx->tech_pvt->downstream_handles) == 0 && ctx->tech_pvt->downstream_buffer) {
      switch_buffer_destroy(&ctx->tech_pvt->downstream_buffer);
      ctx->tech_pvt->downstream_buffer = NULL;
      af_pcm_destroy(&ctx->tech_pvt->downstream_pcm);
    }
    switch_mutex_unlock(ctx->tech_pvt->downstream_mutex);
  }
  if (ctx->session) {
    switch_core_session_rwunlock(ctx->session);
    ctx->session = NULL;
  }
  handle->private_info = NULL;
  return SWITCH_STATUS_SUCCESS;
}
