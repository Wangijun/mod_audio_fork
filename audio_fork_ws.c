#include "audio_fork_ws.h"

#define WS_SILENCE_WATCHDOG_DEFAULT_FRAMES 150 /* 默认连续欠载 150 帧 (3000ms) 触发看门狗 */
#define WS_DRAIN_CONFIRM_FRAMES            3   /* 收到零长帧且读空后，连续 3 帧确认排空方可终止播放 (防吞尾音) */
#define WS_PREBUFFER_DEFAULT_MS            200 /* 默认起播预缓冲 200ms (消灭欠载方波与毛刺) */

switch_status_t audio_fork_ws_file_open(switch_file_handle_t *handle, const char *path) {
  if (!handle || !path) return SWITCH_STATUS_FALSE;

  if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork_ws] 不支持写入模式\n");
    return SWITCH_STATUS_NOTIMPL;
  }

  const char *p = path;
  if (!strncasecmp(p, "audio_fork://", 13)) {
    p += 13;
  }

  char mypath[MAX_PATH_LEN] = {0};
  strncpy(mypath, p, sizeof(mypath) - 1);

  char *query = strchr(mypath, '?');
  if (query) {
    *query = '\0';
    query++;
  }

  char *bugname_part = strchr(mypath, ':');
  char bugname[MAX_BUG_LEN + 1] = {0};
  if (bugname_part) {
    *bugname_part = '\0';
    bugname_part++;
    strncpy(bugname, bugname_part, sizeof(bugname) - 1);
  } else {
    strncpy(bugname, MY_BUG_NAME, sizeof(bugname) - 1);
  }

  const char *uuid = mypath;
  if (zstr(uuid)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork_ws] 缺少会话 UUID 参数: %s\n", path);
    return SWITCH_STATUS_FALSE;
  }

  switch_core_session_t *lsession = switch_core_session_locate(uuid);
  if (!lsession) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[audio_fork_ws] 定位会话失败 UUID: %s\n", uuid);
    return SWITCH_STATUS_FALSE;
  }

  switch_channel_t *channel = switch_core_session_get_channel(lsession);
  switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);
  if (!bug) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(lsession), SWITCH_LOG_ERROR,
      "[audio_fork_ws] 会话 %s 上未挂载监听器: %s\n", uuid, bugname);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }

  private_t *tech_pvt = (private_t *)switch_core_media_bug_get_user_data(bug);
  if (!tech_pvt || !tech_pvt->downstream_mutex || !tech_pvt->downstream_buffer) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(lsession), SWITCH_LOG_ERROR,
      "[audio_fork_ws] 会话 %s media bug 下行缓冲区尚未初始化\n", uuid);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }

  uint32_t samplerate = tech_pvt->downstream_sample_rate ? tech_pvt->downstream_sample_rate : 16000;
  uint32_t channels = tech_pvt->downstream_channels ? tech_pvt->downstream_channels : 1;
  uint32_t watchdog_ms = 3000;
  uint32_t prebuffer_ms = WS_PREBUFFER_DEFAULT_MS;

  if (query) {
    const char *v;
    if ((v = switch_stristr("rate=", query)) || (v = switch_stristr("sampling=", query))) {
      uint32_t r = (uint32_t)atoi(v + (v[0] == 'r' ? 5 : 9));
      if (r >= 8000 && r <= 48000) samplerate = r;
    }
    if ((v = switch_stristr("channels=", query))) {
      uint32_t c = (uint32_t)atoi(v + 9);
      if (c == 1 || c == 2) channels = c;
    }
    if ((v = switch_stristr("watchdog=", query))) {
      watchdog_ms = (uint32_t)atoi(v + 9);
    }
    if ((v = switch_stristr("prebuffer=", query))) {
      uint32_t pb = (uint32_t)atoi(v + 10);
      if (pb > 0 && pb <= 2000) prebuffer_ms = pb;
    }
  }

  uint32_t prebuffer_bytes = (samplerate * channels * (uint32_t)sizeof(int16_t) * prebuffer_ms) / 1000;
  if (prebuffer_bytes < 320) prebuffer_bytes = 320;

  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(audio_fork_ws_ctx_t));
  if (!ctx) {
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_MEMERR;
  }

  ctx->driver_type = AUDIO_FORK_DRIVER_WS;
  ctx->pool = handle->memory_pool;
  ctx->session = lsession;
  ctx->tech_pvt = tech_pvt;
  switch_copy_string(ctx->uuid, uuid, sizeof(ctx->uuid));
  switch_copy_string(ctx->bugname, bugname, sizeof(ctx->bugname));
  ctx->samplerate = samplerate;
  ctx->channels = channels;
  ctx->prebuffer_bytes = prebuffer_bytes;
  ctx->prebuffered = 0;
  ctx->silence_frames = 0;
  ctx->max_silence_frames = (watchdog_ms / 20 > 0) ? (watchdog_ms / 20) : WS_SILENCE_WATCHDOG_DEFAULT_FRAMES;
  ctx->drain_frames = 0;

  /* 重置通道下行控制状态 (保留可能已提前到达的音频流) */
  switch_mutex_lock(tech_pvt->downstream_mutex);
  tech_pvt->downstream_active = 1;
  tech_pvt->downstream_eof = 0;
  tech_pvt->downstream_interrupted = 0;
  tech_pvt->downstream_accept_audio = 1;
  switch_mutex_unlock(tech_pvt->downstream_mutex);

  handle->samplerate = samplerate;
  handle->channels = (uint8_t)channels;
  handle->format = 0;
  handle->sections = 0;
  handle->seekable = 0;
  handle->speed = 0;
  handle->pos = 0;
  handle->private_info = ctx;

  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(lsession), SWITCH_LOG_INFO,
    "[audio_fork_ws] 成功打开 WebSocket 下行播放流: uuid=%s bugname=%s 采样率=%u 声道=%u 预缓冲=%ums (%uB)\n",
    uuid, bugname, samplerate, channels, prebuffer_ms, prebuffer_bytes);

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_ws_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
  if (!handle || !handle->private_info) return SWITCH_STATUS_FALSE;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;
  private_t *tech_pvt = ctx->tech_pvt;
  if (!tech_pvt || !tech_pvt->downstream_mutex || !tech_pvt->downstream_buffer) {
    return SWITCH_STATUS_FALSE;
  }

  /* 1. 瞬时打断拦截 */
  if (tech_pvt->downstream_interrupted) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
      "[audio_fork_ws] 会话 %s 下行播放被打断，立即退出\n", ctx->uuid);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  switch_mutex_lock(tech_pvt->downstream_mutex);

  if (tech_pvt->downstream_interrupted) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  size_t inuse = switch_buffer_inuse(tech_pvt->downstream_buffer);
  int is_eof = tech_pvt->downstream_eof;

  /* 2. 起播预缓冲门控阶段 */
  if (!ctx->prebuffered) {
    if (inuse >= ctx->prebuffer_bytes || is_eof) {
      ctx->prebuffered = 1;
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "[audio_fork_ws] 会话 %s 起播预缓冲达标 (%zu/%u 字节)，开始放音\n",
        ctx->uuid, inuse, ctx->prebuffer_bytes);
    } else {
      ctx->silence_frames++;
      if (ctx->silence_frames >= ctx->max_silence_frames) {
        switch_mutex_unlock(tech_pvt->downstream_mutex);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
          "[audio_fork_ws] 会话 %s 起播预缓冲等待超时看门狗触发 (%u 帧)，退出播放\n",
          ctx->uuid, ctx->silence_frames);
        *len = 0;
        return SWITCH_STATUS_FALSE;
      }
      switch_mutex_unlock(tech_pvt->downstream_mutex);
      switch_yield(10000);
      memset(data, 0, *len * sizeof(int16_t) * ctx->channels);
      return SWITCH_STATUS_SUCCESS;
    }
  }

  /* 3. 正常音频消费阶段 */
  size_t frame_bytes = sizeof(int16_t) * ctx->channels;
  if (inuse > 0) {
    size_t bytes_requested = *len * frame_bytes;
    /* 严格执行整采样边界对齐，杜绝字节错位高频白噪音 */
    size_t safe_inuse = inuse - (inuse % frame_bytes);
    size_t bytes_to_read = bytes_requested < safe_inuse ? bytes_requested : safe_inuse;

    size_t bytes_read = switch_buffer_read(tech_pvt->downstream_buffer, data, bytes_to_read);
    switch_mutex_unlock(tech_pvt->downstream_mutex);

    *len = bytes_read / frame_bytes;
    ctx->silence_frames = 0;
    ctx->drain_frames = 0;
    return SWITCH_STATUS_SUCCESS;
  }

  /* 4. 缓冲区已读空：排空完成判定或欠载看门狗 */
  if (is_eof) {
    ctx->drain_frames++;
    switch_mutex_unlock(tech_pvt->downstream_mutex);

    if (ctx->drain_frames >= WS_DRAIN_CONFIRM_FRAMES) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "[audio_fork_ws] 会话 %s 零长帧定稿并完成尾音排空，优雅返回 EOF\n", ctx->uuid);
      *len = 0;
      return SWITCH_STATUS_FALSE;
    }
    *len = 0;
    return SWITCH_STATUS_SUCCESS;
  }

  /* 欠载静音等待 */
  ctx->silence_frames++;
  if (ctx->silence_frames >= ctx->max_silence_frames) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
      "[audio_fork_ws] 会话 %s 下行播放静音看门狗超时 (%u 帧)，退出播放\n",
      ctx->uuid, ctx->silence_frames);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }

  switch_mutex_unlock(tech_pvt->downstream_mutex);
  switch_yield(10000);
  memset(data, 0, *len * frame_bytes);
  return SWITCH_STATUS_SUCCESS;
}

switch_status_t audio_fork_ws_file_close(switch_file_handle_t *handle) {
  if (!handle || !handle->private_info) return SWITCH_STATUS_SUCCESS;
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)handle->private_info;

  if (ctx->tech_pvt && ctx->tech_pvt->downstream_mutex) {
    switch_mutex_lock(ctx->tech_pvt->downstream_mutex);
    ctx->tech_pvt->downstream_active = 0;
    ctx->tech_pvt->downstream_interrupted = 0;
    ctx->tech_pvt->downstream_eof = 0;
    switch_mutex_unlock(ctx->tech_pvt->downstream_mutex);
  }

  if (ctx->session) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
      "[audio_fork_ws] 释放会话锁 UUID: %s\n", ctx->uuid);
    switch_core_session_rwunlock(ctx->session);
    ctx->session = NULL;
  }

  handle->private_info = NULL;
  return SWITCH_STATUS_SUCCESS;
}
