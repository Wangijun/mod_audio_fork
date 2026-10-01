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
 *    - downstream_partial: 字节对齐缓冲区, 处理跨 WebSocket 碎片边界的奇数字节 (不足 1 个完整 16-bit 采样);
 *    - downstream_interrupted: 即时打断标记, 收到 killAudio 信令后置 1 并清空环形缓冲.
 */

#include "audio_fork_ws.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/* 默认连续欠载 150 帧 (3000ms) 触发看门狗强退 */
#define WS_SILENCE_WATCHDOG_DEFAULT_FRAMES 150
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

/**
 * @brief 回滚增加的下行播放句柄计数 (在 open 流程失败时调用)
 */
static void rollback_downstream_handle(private_t *tech_pvt) {
  if (!tech_pvt || !tech_pvt->downstream_mutex) return;
  switch_mutex_lock(tech_pvt->downstream_mutex);
  unsigned int handles = switch_atomic_read(&tech_pvt->downstream_handles);
  if (handles > 0) --handles;
  switch_atomic_set(&tech_pvt->downstream_handles, handles);
  switch_atomic_set(&tech_pvt->downstream_active, handles > 0 ? 1U : 0U);
  if (handles == 0) {
    switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
  }
  switch_mutex_unlock(tech_pvt->downstream_mutex);
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
  if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
  if (switch_atomic_read(&tech_pvt->lifecycle_state) != AUDIO_FORK_LIFECYCLE_ACTIVE) {
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }

  uint32_t samplerate, channels, session_samplerate, session_channels;
  uint32_t watchdog_ms = 3000, prebuffer_ms = audio_fork_ws_prebuffer_ms;
  uint64_t generation;
  int present = 0;
  int sampling_present = 0;
  uint32_t sampling_value = 0;
  switch_mutex_lock(tech_pvt->downstream_mutex);
  if (!tech_pvt->downstream_buffer) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  samplerate = tech_pvt->downstream_sample_rate ? tech_pvt->downstream_sample_rate : 16000;
  channels = tech_pvt->downstream_channels ? tech_pvt->downstream_channels : 1;
  session_samplerate = samplerate;
  session_channels = channels;

  /* 校验可选参数: rate 与 sampling 别名、channels 与 watchdog */
  if (query && parse_query_uint(query, "rate", 64000, &samplerate, &present) < 0) goto invalid_query;
  if (query && parse_query_uint(query, "sampling", 64000, &sampling_value, &sampling_present) < 0) goto invalid_query;
  if (sampling_present && present && sampling_value != samplerate) goto invalid_query;
  if (sampling_present && !present) { samplerate = sampling_value; present = 1; }
  if (present && (samplerate < 8000 || samplerate > 64000 || samplerate % 8000 != 0 || samplerate != session_samplerate)) goto invalid_query;
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

  /* speak_start 已经推进 generation; open 只记录当前代际, 不能再次推进,
     否则合法的 speak_start -> open 顺序会让句柄立即失效. */
  generation = tech_pvt->downstream_generation;
  switch_atomic_add(&tech_pvt->downstream_handles, 1);
  /* 保留 speak_start/binary 在 open 之前到达的 EOF, 残余字节和缓冲数据;
     新句的接收状态由 speak_start 原子地复位. */
  switch_atomic_set(&tech_pvt->downstream_active, 1);
  switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
  switch_atomic_set(&tech_pvt->downstream_accept_audio, 1);
  switch_atomic_set(&tech_pvt->downstream_overrun_notified, 0);
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

  uint32_t prebuffer_bytes;
  if (!calculate_prebuffer_bytes(samplerate, channels, prebuffer_ms, &prebuffer_bytes)) {
    rollback_downstream_handle(tech_pvt);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(*ctx));
  if (!ctx) {
    rollback_downstream_handle(tech_pvt);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_MEMERR;
  }
  memset(ctx, 0, sizeof(*ctx));
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
  ctx->max_silence_frames = watchdog_ms ? (watchdog_ms / 20U ? watchdog_ms / 20U : 1U) : 0;
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
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
  switch_core_session_rwunlock(lsession);
  return SWITCH_STATUS_FALSE;
}

switch_status_t audio_fork_ws_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
  if (!handle || !handle->private_info || !data || !len || *len == 0) return SWITCH_STATUS_FALSE;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;
  private_t *tech_pvt = ctx->tech_pvt;
  if (!tech_pvt || !tech_pvt->downstream_mutex) { *len = 0; return SWITCH_STATUS_FALSE; }
  const size_t frame_bytes = sizeof(int16_t) * ctx->channels;
  if (frame_bytes == 0 || *len > SIZE_MAX / frame_bytes) { *len = 0; return SWITCH_STATUS_FALSE; }
  size_t bytes_requested = *len * frame_bytes;
  int report_partial = 0;

  switch_mutex_lock(tech_pvt->downstream_mutex);
  /* 检查句柄有效性: 缓冲存在、代际匹配且未被打断 */
  if (!tech_pvt->downstream_buffer ||
      ctx->generation != tech_pvt->downstream_generation ||
      switch_atomic_read(&tech_pvt->downstream_interrupted)) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }
  size_t inuse = switch_buffer_inuse(tech_pvt->downstream_buffer);
  int eof = (int)switch_atomic_read(&tech_pvt->downstream_eof);

  /* 1. 起播预缓冲门控: 未达成预缓冲字节且流尚未结束前, 填充静音帧阻断 */
  if (!ctx->prebuffered && inuse < ctx->prebuffer_bytes && !eof) {
    size_t max_silence_samples = ctx->samplerate ? (ctx->samplerate * 20U / 1000U) : 320U;
    if (*len > max_silence_samples) {
      *len = max_silence_samples;
      bytes_requested = *len * frame_bytes;
    }
    ctx->silence_frames++;
    uint32_t max_silence = ctx->max_silence_frames;
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    memset(data, 0, bytes_requested);
    *len = bytes_requested / frame_bytes;
    handle->sample_count += *len;
    if (max_silence && ctx->silence_frames >= max_silence) { *len = 0; return SWITCH_STATUS_FALSE; }
    return SWITCH_STATUS_SUCCESS;
  }
  ctx->prebuffered = 1;

  /* 2. 正常读取数据: 仅读取对齐整帧的 PCM 数据 */
  size_t safe_inuse = inuse - (inuse % frame_bytes);
  if (safe_inuse > 0) {
    size_t to_read = bytes_requested < safe_inuse ? bytes_requested : safe_inuse;
    size_t bytes_read = switch_buffer_read(tech_pvt->downstream_buffer, data, to_read);
    bytes_read -= bytes_read % frame_bytes;
    if (bytes_read > 0) {
      switch_mutex_unlock(tech_pvt->downstream_mutex);
      *len = bytes_read / frame_bytes;
      handle->sample_count += *len;
      ctx->silence_frames = 0;
      return SWITCH_STATUS_SUCCESS;
    }
  }

  /* 3. 流结束 (EOF) 处理 */
  if (eof) {
    if (tech_pvt->downstream_partial_len ||
        switch_atomic_read(&tech_pvt->downstream_partial_error) ||
        (inuse % frame_bytes) != 0) {
      if (!switch_atomic_read(&tech_pvt->downstream_partial_error_notified)) {
        switch_atomic_set(&tech_pvt->downstream_partial_error_notified, 1);
        report_partial = 1;
      }
    }
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    if (report_partial && ctx->session && tech_pvt->responseHandler) {
      tech_pvt->responseHandler(ctx->session, EVENT_ERROR, (char *)"{\"code\":\"pcm_partial_frame\"}");
    }
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  /* 4. 欠载静音补偿: 缓冲临时读空, 填充静音并递增看门狗 */
  size_t max_silence_samples = ctx->samplerate ? (ctx->samplerate * 20U / 1000U) : 320U;
  if (*len > max_silence_samples) {
    *len = max_silence_samples;
    bytes_requested = *len * frame_bytes;
  }
  ctx->silence_frames++;
  uint32_t max_silence = ctx->max_silence_frames;
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  memset(data, 0, bytes_requested);
  *len = bytes_requested / frame_bytes;
  handle->sample_count += *len;
  if (max_silence && ctx->silence_frames >= max_silence) { *len = 0; return SWITCH_STATUS_FALSE; }
  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_ws_file_close(switch_file_handle_t *handle) {
  if (!handle || !handle->private_info) return SWITCH_STATUS_SUCCESS;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;
  if (ctx->tech_pvt && ctx->tech_pvt->downstream_mutex) {
    switch_mutex_lock(ctx->tech_pvt->downstream_mutex);
    unsigned int handles = switch_atomic_read(&ctx->tech_pvt->downstream_handles);
    if (handles > 0) --handles;
    switch_atomic_set(&ctx->tech_pvt->downstream_handles, handles);
    switch_atomic_set(&ctx->tech_pvt->downstream_active, handles > 0 ? 1U : 0U);
    if (handles == 0) {
      const int current_generation = (ctx->generation == ctx->tech_pvt->downstream_generation);
      switch_atomic_set(&ctx->tech_pvt->downstream_interrupted, current_generation ? 1U : 0U);
      switch_atomic_set(&ctx->tech_pvt->downstream_accept_audio, current_generation ? 0U : 1U);
    }
    if (handles == 0 && ctx->generation == ctx->tech_pvt->downstream_generation) {
      switch_atomic_set(&ctx->tech_pvt->downstream_eof, 0);
      ctx->tech_pvt->downstream_partial_len = 0;
      if (ctx->tech_pvt->downstream_buffer) switch_buffer_zero(ctx->tech_pvt->downstream_buffer);
    }
    if (switch_atomic_read(&ctx->tech_pvt->cleanup_started) &&
        switch_atomic_read(&ctx->tech_pvt->downstream_handles) == 0 && ctx->tech_pvt->downstream_buffer) {
      switch_buffer_destroy(&ctx->tech_pvt->downstream_buffer);
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
