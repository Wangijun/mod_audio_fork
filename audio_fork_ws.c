#include "audio_fork_ws.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define WS_SILENCE_WATCHDOG_DEFAULT_FRAMES 150
#define WS_MAX_WATCHDOG_MS                 600000

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

switch_status_t audio_fork_ws_file_open(switch_file_handle_t *handle, const char *path) {
  if (!handle || !path) return SWITCH_STATUS_FALSE;
  if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) return SWITCH_STATUS_NOTIMPL;

  const char *p = path;
  if (!strncasecmp(p, "audio_fork://", 13)) p += 13;
  size_t path_len = strnlen(p, MAX_PATH_LEN + MAX_BUG_LEN + 3);
  if (path_len == 0 || path_len > MAX_PATH_LEN + MAX_BUG_LEN + 1) return SWITCH_STATUS_FALSE;

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
                      "mod_audio_fork: WebSocket URL 的 prebuffer 参数已被模组忽略，使用 ws-prebuffer-ms=%u ms\n",
                      prebuffer_ms);
  }
  /* speak_start 已经推进 generation；open 只记录当前代际，不能再次推进，
     否则合法的 speak_start -> open 顺序会让句柄立即失效。 */
  generation = tech_pvt->downstream_generation;
  switch_atomic_add(&tech_pvt->downstream_handles, 1);
  /* 保留 speak_start/binary 在 open 之前到达的 EOF、残余字节和缓冲数据；
     新句的接收状态由 speak_start 原子地复位。 */
  switch_atomic_set(&tech_pvt->downstream_active, 1);
  switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
  switch_atomic_set(&tech_pvt->downstream_accept_audio, 1);
  switch_atomic_set(&tech_pvt->downstream_overrun_notified, 0);
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

  uint32_t prebuffer_bytes;
  if (!calculate_prebuffer_bytes(samplerate, channels, prebuffer_ms, &prebuffer_bytes)) {
    switch_mutex_lock(tech_pvt->downstream_mutex);
    unsigned int handles = switch_atomic_read(&tech_pvt->downstream_handles);
    if (handles > 0) --handles;
    switch_atomic_set(&tech_pvt->downstream_handles, handles);
    switch_atomic_set(&tech_pvt->downstream_active, handles > 0 ? 1U : 0U);
    if (handles == 0) switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    switch_core_session_rwunlock(lsession);
    return SWITCH_STATUS_FALSE;
  }
  audio_fork_ws_ctx_t *ctx = (audio_fork_ws_ctx_t *)switch_core_alloc(handle->memory_pool, sizeof(*ctx));
  if (!ctx) {
    switch_mutex_lock(tech_pvt->downstream_mutex);
    unsigned int handles = switch_atomic_read(&tech_pvt->downstream_handles);
    if (handles > 0) --handles;
    switch_atomic_set(&tech_pvt->downstream_handles, handles);
    switch_atomic_set(&tech_pvt->downstream_active, handles > 0 ? 1U : 0U);
    if (handles == 0) switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
    switch_mutex_unlock(tech_pvt->downstream_mutex);
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
                    "mod_audio_fork: WebSocket 播放预缓冲配置 %u ms，本次目标 %u 字节\n",
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
  const size_t bytes_requested = *len * frame_bytes;
  int report_partial = 0;
  switch_mutex_lock(tech_pvt->downstream_mutex);
  if (!tech_pvt->downstream_buffer ||
      ctx->generation != tech_pvt->downstream_generation ||
      switch_atomic_read(&tech_pvt->downstream_interrupted)) {
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }
  size_t inuse = switch_buffer_inuse(tech_pvt->downstream_buffer);
  int eof = (int)switch_atomic_read(&tech_pvt->downstream_eof);
  if (!ctx->prebuffered && inuse < ctx->prebuffer_bytes && !eof) {
    ctx->silence_frames++;
    uint32_t max_silence = ctx->max_silence_frames;
    switch_mutex_unlock(tech_pvt->downstream_mutex);
    memset(data, 0, bytes_requested);
    *len = bytes_requested / frame_bytes;
    if (max_silence && ctx->silence_frames >= max_silence) { *len = 0; return SWITCH_STATUS_FALSE; }
    return SWITCH_STATUS_SUCCESS;
  }
  ctx->prebuffered = 1;
  size_t safe_inuse = inuse - (inuse % frame_bytes);
  if (safe_inuse > 0) {
    size_t to_read = bytes_requested < safe_inuse ? bytes_requested : safe_inuse;
    size_t bytes_read = switch_buffer_read(tech_pvt->downstream_buffer, data, to_read);
    bytes_read -= bytes_read % frame_bytes;
    if (bytes_read > 0) {
      switch_mutex_unlock(tech_pvt->downstream_mutex);
      *len = bytes_read / frame_bytes;
      ctx->silence_frames = 0;
      return SWITCH_STATUS_SUCCESS;
    }
    /* 只有不完整采样帧时先按欠载补静音；等 EOF 再报告协议错误。 */
  }
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
    if (report_partial && ctx->session && tech_pvt->responseHandler)
      tech_pvt->responseHandler(ctx->session, EVENT_ERROR, (char *)"{\"code\":\"pcm_partial_frame\"}");
    *len = 0;
    return SWITCH_STATUS_FALSE;
  }
  ctx->silence_frames++;
  uint32_t max_silence = ctx->max_silence_frames;
  switch_mutex_unlock(tech_pvt->downstream_mutex);
  memset(data, 0, bytes_requested);
  *len = bytes_requested / frame_bytes;
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
