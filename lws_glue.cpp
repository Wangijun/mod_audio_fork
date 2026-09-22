#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <list>
#include <algorithm>
#include <functional>
#include <atomic>
#include <vector>
#include <utility>
#include <cassert>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <regex>
#include <iterator>

#include "parser.hpp"
#include "mod_audio_fork.h"
#include "audio_pipe.hpp"
#include "vector_math.h"
#include "lws_glue.h"

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /* 8kHz 下每 20ms 单声道 320 字节 */
#define BUFFER_GROW_SIZE (16384)



/* ========================================================================= */
/* WebSocket 通信与业务生命周期管理                                           */
/* ========================================================================= */

namespace {
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads __attribute__((unused)) = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static std::atomic<unsigned int> idxCallCount{0};

  static bool sessionBugStillActive(switch_core_session_t* session, private_t* tech_pvt) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    void* activeBug = switch_channel_get_private(channel, tech_pvt->bugname);
    return activeBug && (!tech_pvt->media_bug || activeBug == tech_pvt->media_bug);
  }

  void processIncomingBinary(private_t* tech_pvt, switch_core_session_t* session, const char* data, size_t dataLength) {
    if (!tech_pvt || !session) return;

    if (tech_pvt->downstream_mutex) {
      switch_mutex_lock(tech_pvt->downstream_mutex);
      if (dataLength == 0) {
        tech_pvt->downstream_eof = 1;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
          "(%u) 下行收到零长帧定稿信号，标记 downstream_eof=1\n", tech_pvt->id);
      } else if (tech_pvt->downstream_buffer) {
        if (tech_pvt->downstream_accept_audio) {
          switch_buffer_write(tech_pvt->downstream_buffer, data, dataLength);
        }
      }
      switch_mutex_unlock(tech_pvt->downstream_mutex);
    }
  }

  void processIncomingMessage(private_t* tech_pvt, switch_core_session_t* session, const char* message) {
    std::string msg = message;
    std::string type;
    cJSON* json = parse_json(session, msg, type);
    if (json) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "(%u) processIncomingMessage - 收到 %s 消息: %s\n", tech_pvt->id, type.c_str(), message);
      cJSON* jsonData = cJSON_GetObjectItem(json, "data");

            if (0 == type.compare("killAudio")) {
        if (tech_pvt->downstream_mutex) {
          switch_mutex_lock(tech_pvt->downstream_mutex);
          tech_pvt->downstream_interrupted = 1;
          tech_pvt->downstream_accept_audio = 0;
          tech_pvt->downstream_eof = 0;
          if (tech_pvt->downstream_buffer) {
            switch_buffer_zero(tech_pvt->downstream_buffer);
          }
          switch_mutex_unlock(tech_pvt->downstream_mutex);
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
          "(%u) 收到 killAudio 信令，设置 interrupted=1 并丢弃后续旧音频\n", tech_pvt->id);
        tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, NULL);
      }
      else if (0 == type.compare("speak_start")) {
        if (tech_pvt->downstream_mutex) {
          switch_mutex_lock(tech_pvt->downstream_mutex);
          tech_pvt->downstream_accept_audio = 1;
          tech_pvt->downstream_eof = 0;
          if (!tech_pvt->downstream_active) {
            tech_pvt->downstream_interrupted = 0;
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
              "(%u) 收到 speak_start 文本信令，无活跃播放句柄，安全复位 interrupted=0\n", tech_pvt->id);
          } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
              "(%u) 收到 speak_start 文本信令，当前旧播放仍活跃 (active=1)，保持 interrupted=1 强迫旧播放立即退出\n", tech_pvt->id);
          }
          switch_mutex_unlock(tech_pvt->downstream_mutex);
        }
      }
      else if (0 == type.compare("speak_done")) {
        if (tech_pvt->downstream_mutex) {
          switch_mutex_lock(tech_pvt->downstream_mutex);
          tech_pvt->downstream_eof = 1;
          switch_mutex_unlock(tech_pvt->downstream_mutex);
        }
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
          "(%u) 收到 speak_done 文本信令定稿标记\n", tech_pvt->id);
      }
      else if (0 == type.compare("transcription")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSCRIPTION, jsonString);
        free(jsonString);
      }
      else if (0 == type.compare("transfer")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSFER, jsonString);
        free(jsonString);
      }
      else if (0 == type.compare("disconnect")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_DISCONNECT, jsonString);
        free(jsonString);
      }
      else if (0 == type.compare("error")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_ERROR, jsonString);
        free(jsonString);
      }
      else if (0 == type.compare("json")) {
        char* jsonString = cJSON_PrintUnformatted(json);
        tech_pvt->responseHandler(session, EVENT_JSON, jsonString);
        free(jsonString);
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
          "(%u) processIncomingMessage - 忽略或自定义消息类型: %s\n", tech_pvt->id, type.c_str());
      }
      cJSON_Delete(json);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "(%u) processIncomingMessage - 无法解析 JSON 消息: %s\n", tech_pvt->id, message);
    }
  }

  static void eventCallback(const char* sessionId, const char* bugname, drachtio::AudioPipe::NotifyEvent_t event, const char* message, const char* binary, size_t len) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          if (switch_channel_get_private(channel, bugname) != bug) {
            switch_core_session_rwunlock(session);
            return;
          }
          switch (event) {
            case drachtio::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "连接成功\n");
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
              if (strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "发送初始元数据 %s\n", tech_pvt->initialMetadata);
                if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
                if (switch_channel_get_private(channel, bugname) == bug) {
                  drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
                  if (pAudioPipe) pAudioPipe->bufferForSending(tech_pvt->initialMetadata);
                }
                if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
              }
            break;
            case drachtio::AudioPipe::CONNECT_FAIL:
            {
              std::stringstream jsonStr;
              jsonStr << "{\"reason\":\"" << message << "\"}";
              if (tech_pvt->mutex) {
                switch_mutex_lock(tech_pvt->mutex);
                tech_pvt->pAudioPipe = nullptr;
                switch_mutex_unlock(tech_pvt->mutex);
              } else {
                tech_pvt->pAudioPipe = nullptr;
              }
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) jsonStr.str().c_str());
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "连接失败：%s\n", message);
            }
            break;
            case drachtio::AudioPipe::CONNECTION_DROPPED:
              if (tech_pvt->mutex) {
                switch_mutex_lock(tech_pvt->mutex);
                tech_pvt->pAudioPipe = nullptr;
                switch_mutex_unlock(tech_pvt->mutex);
              } else {
                tech_pvt->pAudioPipe = nullptr;
              }
              tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "远端断开连接\n");
            break;
            case drachtio::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              if (tech_pvt->mutex) {
                switch_mutex_lock(tech_pvt->mutex);
                tech_pvt->pAudioPipe = nullptr;
                switch_mutex_unlock(tech_pvt->mutex);
              } else {
                tech_pvt->pAudioPipe = nullptr;
              }
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "连接已优雅关闭\n");
            break;
            case drachtio::AudioPipe::MESSAGE:
              processIncomingMessage(tech_pvt, session, message);
            break;
            case drachtio::AudioPipe::BINARY:
              processIncomingBinary(tech_pvt, session, binary, len);
            break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }

  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, char * host,
    unsigned int port, char* path, int sslFlags, int sampling, int desiredSampling, int channels,
    char *bugname, char* metadata, responseHandler_t responseHandler) {

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);

    if ((username = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_USERNAME"))) {
      password = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_PASSWORD");
    }

    memset(tech_pvt, 0, sizeof(private_t));

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    strncpy(tech_pvt->host, host, MAX_WS_URL_LEN - 1);
    tech_pvt->host[MAX_WS_URL_LEN - 1] = '\0';
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path, MAX_PATH_LEN - 1);
    tech_pvt->path[MAX_PATH_LEN - 1] = '\0';
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->playout = NULL;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    tech_pvt->audio_paused = 0;
    tech_pvt->graceful_shutdown = 0;
    if (metadata) {
      strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN - 1);
      tech_pvt->initialMetadata[MAX_METADATA_LEN - 1] = '\0';
    }
    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);
    tech_pvt->bugname[MAX_BUG_LEN] = '\0';

    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);
    drachtio::AudioPipe *ap = new drachtio::AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags,
      buflen, read_impl.decoded_bytes_per_packet, username, password, bugname, 1, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配AudioPipe时出错\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);
    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    /* 初始化下行 WebSocket 内存桥环形缓冲区与互斥锁 (最大 128KB 约 4 秒音频) */
    switch_mutex_init(&tech_pvt->downstream_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
    switch_buffer_create_dynamic(&tech_pvt->downstream_buffer, 1024, 16384, 131072);
    tech_pvt->downstream_active = 0;
    tech_pvt->downstream_eof = 0;
    tech_pvt->downstream_interrupted = 0;
    tech_pvt->downstream_accept_audio = 1;
    tech_pvt->downstream_sample_rate = desiredSampling ? (uint32_t)desiredSampling : 16000;
    tech_pvt->downstream_channels = channels ? (uint32_t)channels : 1;

    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) 从 %u 重采样到 %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "初始化重采样器时出错：%s。\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) 此通话无需重采样\n", tech_pvt->id);
    }



    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init 完成\n", tech_pvt->id);
    return SWITCH_STATUS_SUCCESS;
  }

  void destroy_tech_pvt(private_t* tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt->pAudioPipe) {
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      tech_pvt->pAudioPipe = nullptr;
      pAudioPipe->closeAndDestroy();
    }
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->downstream_mutex) {
      switch_mutex_lock(tech_pvt->downstream_mutex);
      if (tech_pvt->downstream_buffer) {
        switch_buffer_destroy(&tech_pvt->downstream_buffer);
        tech_pvt->downstream_buffer = nullptr;
      }
      switch_mutex_unlock(tech_pvt->downstream_mutex);
      tech_pvt->downstream_mutex = nullptr;
    }


  }
}

extern "C" {
  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    int lws_ssl_flags = 0;
    std::string uri = szServerUri;
    std::regex re("^([a-zA-Z]+)://([^:/]+)(?::([0-9]+))?(.*)$");
    std::smatch matches;

    if (std::regex_search(uri, matches, re)) {
      std::string scheme = matches[1];
      std::string h = matches[2];
      std::string p = matches[3];
      std::string pth = matches[4];

      if (scheme == "wss") {
        lws_ssl_flags = LCCSCF_USE_SSL;
      }
      else if (scheme != "ws") {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无效的协议：%s，必须是 ws 或 wss\n", scheme.c_str());
        return 0;
      }

      strcpy(host, h.c_str());
      strcpy(path, pth.length() > 0 ? pth.c_str() : "/");

      if (p.length() > 0) {
        *pPort = atoi(p.c_str());
      }
      else {
        *pPort = (lws_ssl_flags & LCCSCF_USE_SSL) ? 443 : 80;
      }

      const char* allowSelfSigned = switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED");
      if (allowSelfSigned && switch_true(allowSelfSigned)) {
        lws_ssl_flags |= LCCSCF_ALLOW_SELFSIGNED;
      }
      const char* skipServerCertHostnameCheck = switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK");
      if (skipServerCertHostnameCheck && switch_true(skipServerCertHostnameCheck)) {
        lws_ssl_flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
      }
      const char* allowExpired = switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED");
      if (allowExpired && switch_true(allowExpired)) {
        lws_ssl_flags |= LCCSCF_ALLOW_EXPIRED;
      }

      *pSslFlags = lws_ssl_flags;
      return 1;
    }
    return 0;
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      default: break;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }

  switch_status_t fork_init(void) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 音频缓冲（秒）：    %d 秒\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 子协议：              %s\n", mySubProtocolName);

    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    drachtio::AudioPipe::initialize(mySubProtocolName, logs, lws_logger);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 初始化成功\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup(void) {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 正在卸载..\n");
    cleanup = drachtio::AudioPipe::deinitialize();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 卸载状态 %d\n", cleanup);
    if (cleanup == true) {
      return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_service_threads(void) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_init(switch_core_session_t *session,
    responseHandler_t responseHandler,
    uint32_t samples_per_second,
    char *host,
    unsigned int port,
    char *path,
    int sampling,
    int sslFlags,
    int channels,
    char *bugname,
    char* metadata,
    void **ppUserData)
  {
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配内存出错！\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, metadata, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_connect(void **ppUserData) {
    private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->connect();
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    return pAudioPipe ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session, char *bugname, char* text, int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_session_cleanup：无bug %s - websocket连接已关闭\n", bugname);
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_session_cleanup\n", id);

    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);

    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    tech_pvt->pAudioPipe = nullptr;

    {
      switch_media_bug_t *activeBug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (activeBug) {
        switch_channel_set_private(channel, bugname, NULL);
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &activeBug);
        }
      }
    }

    struct playout* playout = tech_pvt->playout;
    while (playout) {
      std::remove(playout->file);
      free(playout->file);
      struct playout *tmp = playout;
      playout = playout->next;
      free(tmp);
    }

    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (pAudioPipe) pAudioPipe->closeAndDestroy();



    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    destroy_tech_pvt(tech_pvt);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%u) fork_session_cleanup：连接已关闭\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_send_text(switch_core_session_t *session, char *bugname, char* text) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_send_text 失败，因为没有bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_pauseresume 失败，因为没有bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_graceful_shutdown 失败，因为没有bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    tech_pvt->graceful_shutdown = 1;

    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->do_graceful_shutdown();
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    size_t available = 0;
    if (!tech_pvt) return SWITCH_TRUE;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (!sessionBugStillActive(session, tech_pvt)) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      if (!tech_pvt->pAudioPipe) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != drachtio::AudioPipe::LWS_CLIENT_CONNECTED) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }

      pAudioPipe->lockAudioBuffer();
      available = pAudioPipe->binarySpaceAvailable();
      if (available < pAudioPipe->binaryMinSpace()) {
        pAudioPipe->unlockAudioBuffer();
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }

      if (!tech_pvt->resampler) {
        switch_frame_t frame;
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            pAudioPipe->binaryWritePtrAdd(frame.datalen);
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
            frame.data = pAudioPipe->binaryWritePtr();
          }
        }
      }
      else {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            spx_uint32_t out_len = available / (sizeof(int16_t) * tech_pvt->channels);
            spx_uint32_t in_len = frame.samples;
            if (out_len == 0) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) 正在丢弃数据包！\n",
                  tech_pvt->id);
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
              }
              break;
            }

            speex_resampler_process_interleaved_int(tech_pvt->resampler,
              (const spx_int16_t *) frame.data,
              (spx_uint32_t *) &in_len,
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              size_t bytes_written = (size_t)out_len * sizeof(int16_t) * tech_pvt->channels;
              pAudioPipe->binaryWritePtrAdd(bytes_written);
              available = pAudioPipe->binarySpaceAvailable();
            }
            if (available < pAudioPipe->binaryMinSpace()) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) 正在丢弃数据包！\n",
                  tech_pvt->id);
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
              }
              break;
            }
          }
        }
      }

      pAudioPipe->unlockAudioBuffer();
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_TRUE;
  }
}
