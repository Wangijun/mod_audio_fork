#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
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
#include <cerrno>
#include <climits>
#include <cstdint>
#include <new>
#include <exception>
#include <cctype>

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
  static int bounded_env_int(const char *value, int fallback, int minimum, int maximum) {
    if (!value || !*value) return fallback;
    errno = 0;
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0') return fallback;
    if (parsed < minimum) return minimum;
    if (parsed > maximum) return maximum;
    return (int)parsed;
  }

  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = bounded_env_int(requestedBufferSecs, 2, 1, 5);
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads __attribute__((unused)) =
    (unsigned int)bounded_env_int(requestedNumServiceThreads, 1, 1, 5);
  static std::atomic<unsigned int> idxCallCount{0};

  struct pending_event {
    drachtio::AudioPipe *pipe = nullptr;
    std::string session_id;
    std::string bugname;
    uint64_t generation = 0;
    drachtio::AudioPipe::NotifyEvent_t event = drachtio::AudioPipe::MESSAGE;
    std::string message;
    std::vector<char> binary;
  };

  static std::mutex event_mutex;
  static std::condition_variable event_cv;
  static std::deque<pending_event> event_queue;
  static std::thread event_thread;
  static std::atomic<bool> event_stopping{true};
  static const size_t MAX_EVENT_QUEUE = 1024;

  static bool checked_mul_size(size_t left, size_t right, size_t *result) {
    if (!result || (left != 0 && right > SIZE_MAX / left)) return false;
    *result = left * right;
    return true;
  }

  static bool checked_add_size(size_t left, size_t right, size_t *result) {
    if (!result || right > SIZE_MAX - left) return false;
    *result = left + right;
    return true;
  }

  static bool calculate_audio_buffer_size(int sampling, int channels, size_t *result) {
    if (!result || sampling < 8000 || sampling > 64000 || (sampling % 8000) != 0 ||
        channels < 1 || channels > 2) return false;
    size_t bytes = 0;
    size_t frame_bytes = 0;
    if (!checked_mul_size((size_t)channels, sizeof(int16_t), &frame_bytes) ||
        !checked_mul_size((size_t)sampling, frame_bytes, &bytes) ||
        !checked_mul_size(bytes, (size_t)nAudioBufferSecs, &bytes) ||
        !checked_add_size(bytes, LWS_PRE, &bytes) || bytes > 2U * 1024U * 1024U) {
      return false;
    }
    *result = bytes;
    return true;
  }

  static bool sessionBugStillActive(switch_core_session_t* session, private_t* tech_pvt) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    void* activeBug = switch_channel_get_private(channel, tech_pvt->bugname);
    return activeBug && (!tech_pvt->media_bug || activeBug == tech_pvt->media_bug);
  }

  void destroy_tech_pvt(private_t *tech_pvt);

  void processIncomingBinary(private_t* tech_pvt, switch_core_session_t* session, const char* data, size_t dataLength) {
    if (!tech_pvt || !session || !tech_pvt->downstream_mutex || (dataLength > 0 && !data)) return;

    bool report_overrun = false;
    switch_mutex_lock(tech_pvt->downstream_mutex);
    if (dataLength == 0) {
      switch_atomic_set(&tech_pvt->downstream_eof, 1);
      switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
      if (tech_pvt->downstream_partial_len != 0) {
        switch_atomic_set(&tech_pvt->downstream_partial_error, 1);
        /* 延迟到 file_read 排空完整帧后再报告协议错误。 */
      }
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
        "(%u) 下行收到零长帧定稿信号，标记 downstream_eof=1\n", tech_pvt->id);
    } else if (tech_pvt->downstream_buffer &&
               switch_atomic_read(&tech_pvt->downstream_accept_audio) &&
               !switch_atomic_read(&tech_pvt->downstream_interrupted) &&
               !switch_atomic_read(&tech_pvt->downstream_eof)) {
      const size_t frame_bytes = (tech_pvt->downstream_channels == 2 ? 4U : 2U);
      size_t offset = 0;

      /* 先补齐上一个 WebSocket fragment 留下的半个采样帧。 */
      if (tech_pvt->downstream_partial_len != 0) {
        size_t need = frame_bytes - tech_pvt->downstream_partial_len;
        size_t take = (dataLength < need) ? dataLength : need;
        memcpy(tech_pvt->downstream_partial + tech_pvt->downstream_partial_len, data, take);
        tech_pvt->downstream_partial_len = (uint8_t)(tech_pvt->downstream_partial_len + take);
        offset += take;
        if (tech_pvt->downstream_partial_len == frame_bytes) {
          size_t before = switch_buffer_inuse(tech_pvt->downstream_buffer);
          switch_buffer_write(tech_pvt->downstream_buffer, tech_pvt->downstream_partial, frame_bytes);
          size_t after = switch_buffer_inuse(tech_pvt->downstream_buffer);
          if (after < before || after - before != frame_bytes) {
            switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
            if (!switch_atomic_read(&tech_pvt->downstream_overrun_notified)) {
              switch_atomic_set(&tech_pvt->downstream_overrun_notified, 1);
              report_overrun = true;
            }
          }
          tech_pvt->downstream_partial_len = 0;
        }
      }

      if (switch_atomic_read(&tech_pvt->downstream_accept_audio) && offset < dataLength) {
        size_t remaining = dataLength - offset;
        size_t complete = remaining - (remaining % frame_bytes);
        if (complete > 0) {
          size_t before = switch_buffer_inuse(tech_pvt->downstream_buffer);
          switch_buffer_write(tech_pvt->downstream_buffer, data + offset, complete);
          size_t after = switch_buffer_inuse(tech_pvt->downstream_buffer);
          if (after < before || after - before != complete) {
            switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
            if (!switch_atomic_read(&tech_pvt->downstream_overrun_notified)) {
              switch_atomic_set(&tech_pvt->downstream_overrun_notified, 1);
              report_overrun = true;
            }
          }
          offset += complete;
        }
        size_t tail = dataLength - offset;
        if (tail > 0 && tail < frame_bytes) {
          memcpy(tech_pvt->downstream_partial, data + offset, tail);
          tech_pvt->downstream_partial_len = (uint8_t)tail;
        }
      }
    }
    switch_mutex_unlock(tech_pvt->downstream_mutex);

    if (report_overrun) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "(%u) 下行 PCM 缓冲区空间不足，停止接收音频\n", tech_pvt->id);
      switch_atomic_set(&tech_pvt->downstream_eof, 1);
      if (tech_pvt->responseHandler) tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
    }
  }

  void processIncomingMessage(private_t* tech_pvt, switch_core_session_t* session, const char* message) {
    if (!tech_pvt || !session || !message) return;
    try {
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
          switch_atomic_set(&tech_pvt->downstream_interrupted, 1);
          switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
          switch_atomic_set(&tech_pvt->downstream_eof, 0);
          tech_pvt->downstream_partial_len = 0;
          if (tech_pvt->downstream_buffer) {
            switch_buffer_zero(tech_pvt->downstream_buffer);
          }
          switch_mutex_unlock(tech_pvt->downstream_mutex);
        }
        cJSON* speakId = jsonData ? cJSON_GetObjectItem(jsonData, "speakId") : NULL;
        const char* speakIdValue = cJSON_IsString(speakId) ? speakId->valuestring : "";
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
          "(%u) 收到 killAudio 信令，speakId=%s，设置 interrupted=1 并丢弃后续旧音频\n",
          tech_pvt->id, speakIdValue);
        char* killPayload = jsonData ? cJSON_PrintUnformatted(jsonData) : NULL;
        tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, killPayload);
        free(killPayload);
      }
      else if (0 == type.compare("speak_start")) {
        if (tech_pvt->downstream_mutex) {
          switch_mutex_lock(tech_pvt->downstream_mutex);
          switch_atomic_set(&tech_pvt->downstream_accept_audio, 1);
          switch_atomic_set(&tech_pvt->downstream_eof, 0);
          tech_pvt->downstream_partial_len = 0;
          /* 每个 speak_start 开启新的语音段；清掉上一段尚未消费的 PCM，
             避免旧播放句柄的尾音泄漏到新代际。 */
          if (tech_pvt->downstream_buffer) switch_buffer_zero(tech_pvt->downstream_buffer);
          switch_atomic_set(&tech_pvt->downstream_partial_error, 0);
          switch_atomic_set(&tech_pvt->downstream_partial_error_notified, 0);
          switch_atomic_set(&tech_pvt->downstream_overrun_notified, 0);
          tech_pvt->downstream_generation++;
          /* 旧句柄通过 generation 不匹配退出。全局 interrupted 必须复位，
             否则旧句柄尚未 close 时到达的新句 PCM 会被误丢弃。 */
          switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "(%u) 收到 speak_start 文本信令，开启新代际并接收新句 PCM（旧播放句柄数=%u）\n",
            tech_pvt->id, switch_atomic_read(&tech_pvt->downstream_handles));
          switch_mutex_unlock(tech_pvt->downstream_mutex);
        }
      }
      else if (0 == type.compare("speak_done")) {
        if (tech_pvt->downstream_mutex) {
          switch_mutex_lock(tech_pvt->downstream_mutex);
          switch_atomic_set(&tech_pvt->downstream_eof, 1);
          switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
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
    } catch (const std::exception& ex) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "(%u) 处理 WebSocket 文本消息失败: %s\n", tech_pvt->id, ex.what());
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "(%u) 处理 WebSocket 文本消息失败\n", tech_pvt->id);
    }
  }

  static void finishPipe(private_t *tech_pvt) {
    if (!tech_pvt) return;
    drachtio::AudioPipe *pipe = nullptr;
    bool owner = false;
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    pipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pipe) {
      tech_pvt->pAudioPipe = nullptr;
      owner = true;
    }
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    if (owner) pipe->releaseOwner();

    /* 清理已开始且没有播放句柄时，LWS close 回调是唯一安全的缓冲销毁点。 */
    if (switch_atomic_read(&tech_pvt->cleanup_started) &&
        !switch_atomic_read(&tech_pvt->downstream_active) &&
        switch_atomic_read(&tech_pvt->downstream_handles) == 0 && tech_pvt->downstream_mutex) {
      switch_mutex_lock(tech_pvt->downstream_mutex);
      if (tech_pvt->downstream_buffer) switch_buffer_destroy(&tech_pvt->downstream_buffer);
      switch_mutex_unlock(tech_pvt->downstream_mutex);
    }
  }

  static void dispatchEvent(const pending_event& pending) {
    switch_core_session_t* session = switch_core_session_locate(pending.session_id.c_str());
    if (!session) return;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, pending.bugname.c_str());
    if (bug) {
      private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
      if (tech_pvt) {
        if (tech_pvt->id != pending.generation || switch_channel_get_private(channel, pending.bugname.c_str()) != bug) {
          switch_core_session_rwunlock(session);
          return;
        }
        if (switch_atomic_read(&tech_pvt->cleanup_started) &&
            pending.event != drachtio::AudioPipe::CONNECTION_CLOSED_GRACEFULLY &&
            pending.event != drachtio::AudioPipe::CONNECTION_DROPPED &&
            pending.event != drachtio::AudioPipe::CONNECT_FAIL) {
          switch_core_session_rwunlock(session);
          return;
        }
        switch (pending.event) {
          case drachtio::AudioPipe::CONNECT_SUCCESS:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "连接成功\n");
            if (tech_pvt->responseHandler)
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
            if (strlen(tech_pvt->initialMetadata) > 0) {
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "发送初始元数据 %s\n", tech_pvt->initialMetadata);
              if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
              if (switch_channel_get_private(channel, pending.bugname.c_str()) == bug) {
                drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
                if (pAudioPipe) pAudioPipe->bufferForSending(tech_pvt->initialMetadata);
              }
              if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
            }
            break;
          case drachtio::AudioPipe::CONNECT_FAIL: {
            std::stringstream jsonStr;
            jsonStr << "{\"reason\":\"" << (pending.message.empty() ? "unknown" : pending.message) << "\"}";
            finishPipe(tech_pvt);
            if (tech_pvt->responseHandler)
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) jsonStr.str().c_str());
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "连接失败：%s\n",
              pending.message.empty() ? "unknown" : pending.message.c_str());
            break;
          }
          case drachtio::AudioPipe::CONNECTION_DROPPED:
            finishPipe(tech_pvt);
            if (tech_pvt->responseHandler)
              tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "远端断开连接\n");
            break;
          case drachtio::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
            finishPipe(tech_pvt);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "连接已优雅关闭\n");
            break;
          case drachtio::AudioPipe::MESSAGE:
            processIncomingMessage(tech_pvt, session,
              pending.message.empty() ? "" : pending.message.c_str());
            break;
          case drachtio::AudioPipe::BINARY:
            processIncomingBinary(tech_pvt, session,
              pending.binary.empty() ? NULL : pending.binary.data(), pending.binary.size());
            break;
        }
      }
    }
    switch_core_session_rwunlock(session);
  }

  static void eventWorkerLoop() {
    for (;;) {
      pending_event pending;
      {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_cv.wait(lock, [] {
          return event_stopping.load(std::memory_order_acquire) || !event_queue.empty();
        });
        if (event_queue.empty()) {
          if (event_stopping.load(std::memory_order_acquire)) return;
          continue;
        }
        pending = std::move(event_queue.front());
        event_queue.pop_front();
      }
      try {
        dispatchEvent(pending);
      } catch (const std::exception& ex) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
          "mod_audio_fork 事件处理异常: %s\n", ex.what());
      } catch (...) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
          "mod_audio_fork 事件处理异常\n");
      }
      if (pending.pipe) pending.pipe->release();
    }
  }

  static bool startEventWorker() {
    std::lock_guard<std::mutex> lock(event_mutex);
    if (event_thread.joinable()) return true;
    event_stopping.store(false, std::memory_order_release);
    try {
      event_thread = std::thread(eventWorkerLoop);
      return true;
    } catch (const std::exception& ex) {
      event_stopping.store(true, std::memory_order_release);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "mod_audio_fork 事件线程启动失败: %s\n", ex.what());
    } catch (...) {
      event_stopping.store(true, std::memory_order_release);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "mod_audio_fork 事件线程启动失败\n");
    }
    return false;
  }

  static void stopEventWorker() {
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      event_stopping.store(true, std::memory_order_release);
      /* 模块卸载不再向 FreeSWITCH 投递事件；释放队列中的 pipe 引用，
         AudioPipe::deinitialize 随后负责 owner/wsi 引用的最终收尾。 */
      for (pending_event& pending : event_queue) {
        if (pending.pipe) pending.pipe->release();
      }
      event_queue.clear();
    }
    event_cv.notify_all();
    if (event_thread.joinable()) event_thread.join();
  }

  static void eventCallback(drachtio::AudioPipe *pipe, const char* sessionId, const char* bugname, uint64_t generation,
    drachtio::AudioPipe::NotifyEvent_t event, const char* message, const char* binary, size_t len) {
    if (!pipe || !sessionId || !bugname) return;
    /* len 对 MESSAGE/CONNECT_FAIL 表示文本片段或错误文本长度；只有
       BINARY 事件要求 binary 指针与长度配对。 */
    if (event == drachtio::AudioPipe::BINARY && len > 0 && !binary) return;
    pipe->addRef();
    try {
      pending_event pending;
      pending.pipe = pipe;
      pending.session_id = sessionId;
      pending.bugname = bugname;
      pending.generation = generation;
      pending.event = event;
      if (message) {
        if (event == drachtio::AudioPipe::CONNECT_FAIL && len > 0)
          pending.message.assign(message, message + len);
        else
          pending.message = message;
      }
      if (binary && len > 0) pending.binary.assign(binary, binary + len);
      {
        std::lock_guard<std::mutex> lock(event_mutex);
        if (event_stopping.load(std::memory_order_acquire)) {
          pipe->release();
          return;
        }
        if (event_queue.size() >= MAX_EVENT_QUEUE) {
          /* PCM 和普通文本是可丢弃的；关闭/失败控制事件必须保留，
             否则 FreeSWITCH 侧可能永远持有 AudioPipe owner 引用。 */
          auto evict = std::find_if(event_queue.begin(), event_queue.end(),
            [](const pending_event& queued) {
              return queued.event == drachtio::AudioPipe::BINARY ||
                     queued.event == drachtio::AudioPipe::MESSAGE;
            });
          if (evict != event_queue.end()) {
            if (evict->pipe) evict->pipe->release();
            event_queue.erase(evict);
          } else if (event == drachtio::AudioPipe::BINARY ||
                     event == drachtio::AudioPipe::MESSAGE) {
            pipe->release();
            return;
          }
          /* 队列里只剩控制事件时，允许当前控制事件入队，优先保证
             CONNECT_FAIL / CONNECTION_CLOSED 等终止通知不被静默丢弃。 */
        }
        event_queue.emplace_back(std::move(pending));
      }
      event_cv.notify_one();
    } catch (...) {
      /* 回调位于 LWS 服务线程，不能让分配异常穿透 libwebsockets。 */
      pipe->release();
    }
  }

  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, char * host,
    unsigned int port, char* path, int sslFlags, int sampling, int desiredSampling, int channels,
    char *bugname, char* metadata, responseHandler_t responseHandler) {

    if (!tech_pvt || !session || !responseHandler || !host || !path || !bugname ||
        desiredSampling < 8000 || desiredSampling > 64000 ||
        (desiredSampling % 8000) != 0 || channels < 1 || channels > 2 ||
        sampling <= 0 || port == 0 || port > 65535) {
      return SWITCH_STATUS_FALSE;
    }
    const size_t host_len = strnlen(host, MAX_WS_URL_LEN);
    const size_t path_len = strnlen(path, MAX_PATH_LEN);
    const size_t bugname_len = strnlen(bugname, MAX_BUG_LEN + 1);
    if (host_len == 0 || host_len >= MAX_WS_URL_LEN || path_len == 0 ||
        path_len >= MAX_PATH_LEN || bugname_len == 0 || bugname_len > MAX_BUG_LEN) {
      return SWITCH_STATUS_FALSE;
    }
    for (size_t i = 0; i < host_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(host[i]);
      if (c < 0x20 || c == 0x7f) return SWITCH_STATUS_FALSE;
    }
    for (size_t i = 0; i < path_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(path[i]);
      if (c < 0x20 || c == 0x7f) return SWITCH_STATUS_FALSE;
    }

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (switch_core_session_get_read_impl(session, &read_impl) != SWITCH_STATUS_SUCCESS ||
        read_impl.decoded_bytes_per_packet == 0) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无法获取读取 codec 参数\n");
      return SWITCH_STATUS_FALSE;
    }

    if ((username = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_USERNAME"))) {
      password = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_PASSWORD");
    }

    memset(tech_pvt, 0, sizeof(private_t));

    switch_copy_string(tech_pvt->sessionId, switch_core_session_get_uuid(session), sizeof(tech_pvt->sessionId));
    switch_copy_string(tech_pvt->host, host, sizeof(tech_pvt->host));
    tech_pvt->port = port;
    switch_copy_string(tech_pvt->path, path, sizeof(tech_pvt->path));
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->playout = NULL;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    switch_atomic_set(&tech_pvt->buffer_overrun_notified, 0);
    switch_atomic_set(&tech_pvt->audio_paused, 0);
    switch_atomic_set(&tech_pvt->graceful_shutdown, 0);
    switch_atomic_set(&tech_pvt->cleanup_started, 0);
    switch_atomic_set(&tech_pvt->lifecycle_state, AUDIO_FORK_LIFECYCLE_ACTIVE);
    if (metadata) {
      switch_copy_string(tech_pvt->initialMetadata, metadata, sizeof(tech_pvt->initialMetadata));
    }
    switch_copy_string(tech_pvt->bugname, bugname, sizeof(tech_pvt->bugname));

    size_t buflen = 0;
    if (!calculate_audio_buffer_size(desiredSampling, channels, &buflen)) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "(%u) 音频缓冲区大小非法或溢出: rate=%d channels=%d\n", tech_pvt->id, desiredSampling, channels);
      return SWITCH_STATUS_FALSE;
    }
    drachtio::AudioPipe *ap = nullptr;
    try {
      ap = new (std::nothrow) drachtio::AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags,
        buflen, read_impl.decoded_bytes_per_packet, username, password, bugname, tech_pvt->id, 1, eventCallback);
    } catch (const std::exception &ex) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "AudioPipe 构造失败: %s\n", ex.what());
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AudioPipe 构造失败\n");
    }
    if (!ap || !ap->isValid()) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配AudioPipe时出错\n");
      if (ap) ap->closeAndDestroy();
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);
    if (switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
      ap->closeAndDestroy();
      tech_pvt->pAudioPipe = nullptr;
      return SWITCH_STATUS_GENERR;
    }

    /* 初始化下行 WebSocket 内存桥环形缓冲区与互斥锁（最大 2 MiB，覆盖 5 秒 64kHz 双声道预缓冲） */
    if (switch_mutex_init(&tech_pvt->downstream_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS ||
        switch_buffer_create_dynamic(&tech_pvt->downstream_buffer, 4096, 16384, 2U * 1024U * 1024U) != SWITCH_STATUS_SUCCESS) {
      ap->closeAndDestroy();
      tech_pvt->pAudioPipe = nullptr;
      return SWITCH_STATUS_MEMERR;
    }
    switch_atomic_set(&tech_pvt->downstream_active, 0);
    switch_atomic_set(&tech_pvt->downstream_eof, 0);
    switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
    switch_atomic_set(&tech_pvt->downstream_accept_audio, 1);
    switch_atomic_set(&tech_pvt->downstream_partial_error, 0);
    switch_atomic_set(&tech_pvt->downstream_partial_error_notified, 0);
    switch_atomic_set(&tech_pvt->downstream_overrun_notified, 0);
    switch_atomic_set(&tech_pvt->downstream_handles, 0);
    tech_pvt->downstream_partial_len = 0;
    tech_pvt->downstream_generation = 0;
    tech_pvt->downstream_sample_rate = desiredSampling ? (uint32_t)desiredSampling : 16000;
    tech_pvt->downstream_channels = channels ? (uint32_t)channels : 1;

    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) 从 %u 重采样到 %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "初始化重采样器时出错：%s。\n", speex_resampler_strerror(err));
        destroy_tech_pvt(tech_pvt);
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
    if (!tech_pvt) return;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    drachtio::AudioPipe *pending_pipe = nullptr;
    if (tech_pvt->pAudioPipe) {
      pending_pipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      tech_pvt->pAudioPipe = nullptr;
      pending_pipe->closeAndDestroy();
    }
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
    switch_atomic_set(&tech_pvt->downstream_interrupted, 1);
    for (unsigned int wait_ms = 0;
         wait_ms < 2000 && switch_atomic_read(&tech_pvt->downstream_handles) != 0;
         wait_ms += 10) {
      switch_yield(10000);
    }
    if (tech_pvt->downstream_mutex) {
      switch_mutex_lock(tech_pvt->downstream_mutex);
      if (tech_pvt->downstream_buffer && switch_atomic_read(&tech_pvt->downstream_handles) == 0) {
        switch_buffer_destroy(&tech_pvt->downstream_buffer);
        tech_pvt->downstream_buffer = nullptr;
      }
      switch_mutex_unlock(tech_pvt->downstream_mutex);
    }
    switch_atomic_set(&tech_pvt->lifecycle_state, AUDIO_FORK_LIFECYCLE_CLOSED);


  }
}

extern "C" {
  void fork_data_destroy(private_t *tech_pvt) {
    destroy_tech_pvt(tech_pvt);
  }

  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    if (!szServerUri || !host || !path || !pPort || !pSslFlags) return 0;
    try {
    const size_t uri_len = strnlen(szServerUri, MAX_WS_URI_LEN + 1);
    if (uri_len == 0 || uri_len > MAX_WS_URI_LEN) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WebSocket URI 超出长度限制\n");
      return 0;
    }
    for (size_t i = 0; i < uri_len; ++i) {
      unsigned char c = (unsigned char)szServerUri[i];
      if (c < 0x20 || c == 0x7f) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WebSocket URI 包含控制字符\n");
        return 0;
      }
    }

    const char *scheme_end = strstr(szServerUri, "://");
    if (!scheme_end || scheme_end == szServerUri || (size_t)(scheme_end - szServerUri) > 8) return 0;
    std::string scheme(szServerUri, (size_t)(scheme_end - szServerUri));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    int lws_ssl_flags = 0;
    if (scheme == "wss" || scheme == "https") lws_ssl_flags = LCCSCF_USE_SSL;
    else if (scheme != "ws" && scheme != "http") {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无效的协议：%s，必须是 ws/wss 或 http/https\n", scheme.c_str());
      return 0;
    }

    const char *authority = scheme_end + 3;
    const char *path_start = strpbrk(authority, "/?");
    const char *authority_end = path_start ? path_start : szServerUri + uri_len;
    if (authority_end <= authority) return 0;
    std::string authority_text(authority, (size_t)(authority_end - authority));
    std::string h;
    std::string port_text;
    bool has_port = false;
    if (authority_text.front() == '[') {
      size_t close = authority_text.find(']');
      if (close == std::string::npos || close == 1) return 0;
      h = authority_text.substr(1, close - 1);
      if (close + 1 < authority_text.size()) {
        if (authority_text[close + 1] != ':') return 0;
        has_port = true;
        port_text = authority_text.substr(close + 2);
      }
    } else {
      size_t colon = authority_text.rfind(':');
      if (colon != std::string::npos) {
        if (authority_text.find(':') != colon) return 0; /* 未加括号的 IPv6 */
        h = authority_text.substr(0, colon);
        has_port = true;
        port_text = authority_text.substr(colon + 1);
      } else {
        h = authority_text;
      }
    }
    if (h.empty() || h.size() >= MAX_WS_URL_LEN || h.find_first_of("\t\r\n @/?#%\\") != std::string::npos) return 0;
    const char *path_src = path_start ? path_start : "/";
    size_t raw_path_len = path_start ? (size_t)((szServerUri + uri_len) - path_start) : 1;
    bool add_path_slash = path_start && *path_start != '/';
    size_t path_len = raw_path_len + (add_path_slash ? 1U : 0U);
    if (raw_path_len == 0 || path_len >= MAX_PATH_LEN) return 0;
    if (has_port) {
      if (port_text.empty()) return 0;
      for (char c : port_text) {
        if (c < '0' || c > '9') return 0;
      }
      errno = 0;
      char *end = nullptr;
      unsigned long parsed = std::strtoul(port_text.c_str(), &end, 10);
      if (errno == ERANGE || end == port_text.c_str() || *end != '\0' || parsed == 0 || parsed > 65535UL) return 0;
      *pPort = (unsigned int)parsed;
    } else {
      *pPort = (lws_ssl_flags & LCCSCF_USE_SSL) ? 443U : 80U;
    }
    memcpy(host, h.data(), h.size());
    host[h.size()] = '\0';
    if (add_path_slash) {
      path[0] = '/';
      memcpy(path + 1, path_src, raw_path_len);
    } else {
      memcpy(path, path_src, raw_path_len);
    }
    path[path_len] = '\0';

      const char* allowSelfSigned = channel ? switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED") : nullptr;
      if (allowSelfSigned && switch_true(allowSelfSigned)) {
        lws_ssl_flags |= LCCSCF_ALLOW_SELFSIGNED;
      }
      const char* skipServerCertHostnameCheck = channel ? switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK") : nullptr;
      if (skipServerCertHostnameCheck && switch_true(skipServerCertHostnameCheck)) {
        lws_ssl_flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
      }
      const char* allowExpired = channel ? switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED") : nullptr;
      if (allowExpired && switch_true(allowExpired)) {
        lws_ssl_flags |= LCCSCF_ALLOW_EXPIRED;
      }

      *pSslFlags = lws_ssl_flags;
      return 1;
    } catch (const std::exception& ex) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "WebSocket URI 解析失败: %s\n", ex.what());
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WebSocket URI 解析失败\n");
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
    if (!startEventWorker()) {
      return SWITCH_STATUS_FALSE;
    }
    if (!drachtio::AudioPipe::initialize(mySubProtocolName, logs, lws_logger)) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_audio_fork: LWS 服务线程初始化失败\n");
      stopEventWorker();
      return SWITCH_STATUS_FALSE;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 初始化成功\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup(void) {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 正在卸载..\n");
    /* 模块卸载时先停止业务事件线程，避免 LWS 收尾回调在模块代码卸载
       期间重新进入 FreeSWITCH；deinitialize 随后释放所有 pipe 引用。 */
    stopEventWorker();
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
    if (!session || !ppUserData || !responseHandler || !host || !path || !bugname ||
        port == 0 || port > 65535) {
      return SWITCH_STATUS_FALSE;
    }
    const size_t host_len = strnlen(host, MAX_WS_URL_LEN);
    const size_t path_len = strnlen(path, MAX_PATH_LEN);
    const size_t bugname_len = strnlen(bugname, MAX_BUG_LEN + 1);
    if (host_len == 0 || host_len >= MAX_WS_URL_LEN || path_len == 0 ||
        path_len >= MAX_PATH_LEN || bugname_len == 0 || bugname_len > MAX_BUG_LEN) {
      return SWITCH_STATUS_FALSE;
    }
    for (size_t i = 0; i < host_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(host[i]);
      if (c < 0x20 || c == 0x7f) return SWITCH_STATUS_FALSE;
    }
    for (size_t i = 0; i < path_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(path[i]);
      if (c < 0x20 || c == 0x7f) return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配内存出错！\n");
      return SWITCH_STATUS_FALSE;
    }
    /* fork_data_init 可能在读取 codec 前失败，先清零以保证失败收尾安全。 */
    memset(tech_pvt, 0, sizeof(*tech_pvt));

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, metadata, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_connect(void **ppUserData) {
    if (!ppUserData || !*ppUserData) return SWITCH_STATUS_FALSE;
    private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
    bool accepted = pAudioPipe && pAudioPipe->connect();
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    return accepted ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session, switch_media_bug_t *bug, char* text, int channelIsClosing) {
    if (!session || !bug) return SWITCH_STATUS_FALSE;
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    uint32_t id = tech_pvt->id;
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    if (switch_atomic_read(&tech_pvt->lifecycle_state) != AUDIO_FORK_LIFECYCLE_ACTIVE ||
        switch_atomic_read(&tech_pvt->cleanup_started)) {
      if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
      return SWITCH_STATUS_SUCCESS;
    }
    switch_atomic_set(&tech_pvt->cleanup_started, 1);
    switch_atomic_set(&tech_pvt->lifecycle_state, AUDIO_FORK_LIFECYCLE_CLOSING);
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_session_cleanup\n", id);

    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    if (channel && switch_channel_get_private(channel, tech_pvt->bugname) == bug) {
      switch_channel_set_private(channel, tech_pvt->bugname, NULL);
    }
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    tech_pvt->pAudioPipe = nullptr;
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);

    switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
    switch_atomic_set(&tech_pvt->downstream_interrupted, 1);
    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (pAudioPipe) pAudioPipe->closeAndDestroy();

    if (!channelIsClosing) {
      switch_core_media_bug_remove(session, &bug);
    }
    destroy_tech_pvt(tech_pvt);
    switch_atomic_set(&tech_pvt->lifecycle_state, AUDIO_FORK_LIFECYCLE_CLOSED);
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
    switch_atomic_set(&tech_pvt->audio_paused, pause ? 1U : 0U);
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

    switch_atomic_set(&tech_pvt->graceful_shutdown, 1);

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
    if (switch_atomic_read(&tech_pvt->audio_paused) || switch_atomic_read(&tech_pvt->cleanup_started)) return SWITCH_TRUE;

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
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = data;
        frame.buflen = sizeof(data);
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen > 0) {
            size_t writable = pAudioPipe->binarySpaceAvailable();
            size_t to_write = frame.datalen < writable ? frame.datalen : writable;
            if (to_write > 0) {
              memcpy(pAudioPipe->binaryWritePtr(), frame.data, to_write);
              pAudioPipe->binaryWritePtrAdd(to_write);
            }
            if (to_write < frame.datalen) {
              if (!switch_atomic_read(&tech_pvt->buffer_overrun_notified)) {
                switch_atomic_set(&tech_pvt->buffer_overrun_notified, 1);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                  "(%u) 上行 AudioPipe 缓冲区空间不足，丢弃音频\n", tech_pvt->id);
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
              }
              break;
            }
            memset(&frame, 0, sizeof(frame));
            frame.data = data;
            frame.buflen = sizeof(data);
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
              if (!switch_atomic_read(&tech_pvt->buffer_overrun_notified)) {
                switch_atomic_set(&tech_pvt->buffer_overrun_notified, 1);
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
              if (!switch_atomic_read(&tech_pvt->buffer_overrun_notified)) {
                switch_atomic_set(&tech_pvt->buffer_overrun_notified, 1);
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
