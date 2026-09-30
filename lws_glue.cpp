/**
 * @file lws_glue.cpp
 * @brief FreeSWITCH 与 WebSocket 传输层 (AudioPipe) 交互粘合层实现
 *
 * 核心架构与数据流:
 * 1. 上行数据流 (Interception & Upstream):
 *    - FreeSWITCH Media Bug (capture_callback) -> fork_frame()
 *    - 若会话采样率 != 目标采样率, 由 SpeexResampler 转换 (如 8k -> 16k)
 *    - 写入 AudioPipe 环形发送缓冲 (留出 LWS_PRE 头部)
 *    - 由 libwebsockets serviceThread 在 LWS_CALLBACK_CLIENT_WRITEABLE 中推往远端 ASR 服务
 *
 * 2. 下行数据流 (Downstream PCM & 信令):
 *    - libwebsockets serviceThread 收到数据 -> eventCallback() 入队 (event_queue)
 *    - 唤醒独立的内部事件工作线程 (eventWorkerLoop)
 *    - dispatchEvent() 派发:
 *      * BINARY 音频帧 -> processIncomingBinary() -> downstream_buffer (带跨帧奇数字节拼装)
 *      * JSON 文本帧 -> processIncomingMessage() 处理 8 类业务信令
 *    - FreeSWITCH 下行播放驱动 (audio_fork_ws_file_read) 从 downstream_buffer 读取并混音起播
 *
 * 3. 打断 (Barge-In) 与代际隔离 (Generations):
 *    - killAudio 信令: 即刻将 downstream_interrupted 置 1, 丢弃所有未播音频并通知 ESL
 *    - speak_start 信令: 开启新一代 (generation++), 清除上一句尾音, 旧播放句柄自动失效退出
 */

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

  /**
   * @brief 统一通知上行音频缓冲区溢出事件
   *
   * 避免由于 ASR 服务端网络延迟或断连导致上行缓冲塞满时重复记录洪泛日志.
   */
  static void notify_upstream_buffer_overrun(switch_core_session_t *session, private_t *tech_pvt) {
    if (!tech_pvt) return;
    if (!switch_atomic_read(&tech_pvt->buffer_overrun_notified)) {
      switch_atomic_set(&tech_pvt->buffer_overrun_notified, 1);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
        "(%u) 上行 AudioPipe 缓冲区空间不足, 丢弃音频数据包\n", tech_pvt->id);
      if (tech_pvt->responseHandler) {
        tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
      }
    }
  }

  void destroy_tech_pvt(private_t *tech_pvt);

  /**
   * @brief 处理 WebSocket 接收到的下行裸 PCM 二进制音频帧
   *
   * 核心机制:
   * 1. 零长帧定稿: 若 dataLength == 0, 视为远端发送的音频结束标记 (EOF), 设置 downstream_eof = 1;
   * 2. 跨帧碎片对齐: 若上次接收留下不足 1 个完整采样的残余字节 (如单声道 1 字节, 双声道 1~3 字节),
   *    优先使用本次数据头拼齐写入, 杜绝音频相位错位或杂音;
   * 3. 采样整帧写入: 计算 complete = remaining - (remaining % frame_bytes), 保证写入环形缓冲的均为完整采样;
   * 4. 尾部碎片暂存: 将剩余不足 frame_bytes 的尾巴存入 downstream_partial, 留待下一帧拼装.
   */
  void processIncomingBinary(private_t* tech_pvt, switch_core_session_t* session, const char* data, size_t dataLength) {
    if (!tech_pvt || !session || !tech_pvt->downstream_mutex || (dataLength > 0 && !data)) return;

    bool report_overrun = false;
    switch_mutex_lock(tech_pvt->downstream_mutex);
    if (dataLength == 0) {
      switch_atomic_set(&tech_pvt->downstream_eof, 1);
      switch_atomic_set(&tech_pvt->downstream_accept_audio, 0);
      if (tech_pvt->downstream_partial_len != 0) {
        switch_atomic_set(&tech_pvt->downstream_partial_error, 1);
        /* 延迟到 file_read 排空完整帧后再报告协议错误. */
      }
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
        "(%u) 下行收到零长帧定稿信号, 标记 downstream_eof=1\n", tech_pvt->id);
    } else if (tech_pvt->downstream_buffer &&
               switch_atomic_read(&tech_pvt->downstream_accept_audio) &&
               !switch_atomic_read(&tech_pvt->downstream_interrupted) &&
               !switch_atomic_read(&tech_pvt->downstream_eof)) {
      const size_t frame_bytes = (tech_pvt->downstream_channels == 2 ? 4U : 2U);
      size_t offset = 0;

      /* 先补齐上一个 WebSocket fragment 留下的半个采样帧. */
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
        "(%u) 下行 PCM 缓冲区空间不足, 停止接收音频\n", tech_pvt->id);
      switch_atomic_set(&tech_pvt->downstream_eof, 1);
      if (tech_pvt->responseHandler) tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
    }
  }

  /**
   * @brief 解析并分发 WebSocket 下行 JSON 控制信令
   *
   * 支持处理以下 8 种消息类型:
   * 1. killAudio: 即时打断下行播放, 清空环形缓冲并通知 ESL 业务层;
   * 2. speak_start: 开始播报新句子, 推进代际 (generation++), 清除上一句残留尾音;
   * 3. speak_done: 句子播报结束标记 (EOF);
   * 4. transcription: 实时 ASR 转写识别结果;
   * 5. transfer: 呼叫转接控制信令;
   * 6. disconnect: 挂断会话信令;
   * 7. error: 远端错误通知;
   * 8. json: 其他透传的自定义 JSON 载荷.
   */
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
          "(%u) 收到 killAudio 信令, speakId=%s, 设置 interrupted=1 并丢弃后续旧音频\n",
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
          /* 每个 speak_start 开启新的语音段; 清掉上一段尚未消费的 PCM,
             避免旧播放句柄的尾音泄漏到新代际. */
          if (tech_pvt->downstream_buffer) switch_buffer_zero(tech_pvt->downstream_buffer);
          switch_atomic_set(&tech_pvt->downstream_partial_error, 0);
          switch_atomic_set(&tech_pvt->downstream_partial_error_notified, 0);
          switch_atomic_set(&tech_pvt->downstream_overrun_notified, 0);
          tech_pvt->downstream_generation++;
          /* 旧句柄通过 generation 不匹配退出. 全局 interrupted 必须复位,
             否则旧句柄尚未 close 时到达的新句 PCM 会被误丢弃. */
          switch_atomic_set(&tech_pvt->downstream_interrupted, 0);
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
            "(%u) 收到 speak_start 文本信令, 开启新代际并接收新句 PCM(旧播放句柄数=%u)\n",
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

  /**
   * @brief 终止 AudioPipe 连接并释放所属私有数据引用
   *
   * 清理步骤:
   * 1. 加锁后置空 tech_pvt->pAudioPipe 并记录所有权状态;
   * 2. 调用 pipe->releaseOwner() 递减所有者计数;
   * 3. 检查下行播放状态: 若已处于清理阶段且无活跃播放句柄, 销毁下行环形缓冲区.
   *
   * @param tech_pvt 会话私有数据结构体指针
   */
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

    /* 清理已开始且没有播放句柄时, LWS close 回调是唯一安全的缓冲销毁点. */
    if (switch_atomic_read(&tech_pvt->cleanup_started) &&
        !switch_atomic_read(&tech_pvt->downstream_active) &&
        switch_atomic_read(&tech_pvt->downstream_handles) == 0 && tech_pvt->downstream_mutex) {
      switch_mutex_lock(tech_pvt->downstream_mutex);
      if (tech_pvt->downstream_buffer) switch_buffer_destroy(&tech_pvt->downstream_buffer);
      switch_mutex_unlock(tech_pvt->downstream_mutex);
    }
  }

  /**
   * @brief 从事件工作线程向 FreeSWITCH 会话分发就绪事件
   *
   * 核心逻辑:
   * 1. 通过 sessionId 定位并锁定 switch_core_session_t;
   * 2. 校验 bugname 与代际 generation, 防止跨会话或已重建通道事件错乱;
   * 3. 按事件类型调用 responseHandler (CONNECT_SUCCESS / CONNECT_FAIL / DISCONNECT 等)
   *    或流转至 processIncomingMessage / processIncomingBinary;
   * 4. 在 CONNECT_SUCCESS 时若存在 initialMetadata 则立即触发发送.
   *
   * @param pending 待分发的排队事件包
   */
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
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "连接失败: %s\n",
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

  /**
   * @brief 事件工作线程执行函数
   *
   * 采用条件变量 event_cv 进行阻塞等待. 当队列中有事件或收到退出通知时唤醒;
   * 逐个弹出 pending_event 并调用 dispatchEvent 向 FreeSWITCH 投递, 处理完成后
   * 释放对应的 AudioPipe 引用计数.
   */
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

  /**
   * @brief 启动事件分发工作线程
   *
   * @return true 启动成功或已在运行, false 线程启动失败
   */
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

  /**
   * @brief 优雅停止事件分发工作线程
   *
   * 将停止标志 event_stopping 置为 true, 清空队列中积压的待处理事件并释放持有的 pipe 引用,
   * 广播唤醒工作线程并执行 join 等待其退出.
   */
  static void stopEventWorker() {
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      event_stopping.store(true, std::memory_order_release);
      /* 模块卸载不再向 FreeSWITCH 投递事件; 释放队列中的 pipe 引用,
         AudioPipe::deinitialize 随后负责 owner/wsi 引用的最终收尾. */
      for (pending_event& pending : event_queue) {
        if (pending.pipe) pending.pipe->release();
      }
      event_queue.clear();
    }
    event_cv.notify_all();
    if (event_thread.joinable()) event_thread.join();
  }

  /**
   * @brief libwebsockets 事件底层接收回调 (运行于 LWS 服务线程)
   *
   * 职责:
   * 1. 拦截 LWS 抛出的连接、消息与二进制帧事件;
   * 2. 对 pipe 增加引用计数, 打包为 pending_event 压入事件队列;
   * 3. 背压与溢出淘汰策略: 当事件队列达到 MAX_EVENT_QUEUE 深度时, 优先淘汰
   *    旧的 BINARY/MESSAGE 媒体帧, 绝对保护 CONNECT_FAIL / DISCONNECT 等生命周期事件;
   * 4. 捕获所有 C++ 异常, 杜绝抛出导致 LWS 服务主循环崩溃.
   *
   * @param pipe 所属 AudioPipe 实例
   * @param sessionId 绑定的 FreeSWITCH Session UUID
   * @param bugname 关联的 Media Bug 名称
   * @param generation 管道当前代际编号
   * @param event 事件类型枚举
   * @param message 文本消息载荷 (如有)
   * @param binary 二进制数据指针 (如有)
   * @param len 数据长度
   */
  static void eventCallback(drachtio::AudioPipe *pipe, const char* sessionId, const char* bugname, uint64_t generation,
    drachtio::AudioPipe::NotifyEvent_t event, const char* message, const char* binary, size_t len) {
    if (!pipe || !sessionId || !bugname) return;
    /* len 对 MESSAGE/CONNECT_FAIL 表示文本片段或错误文本长度; 只有
       BINARY 事件要求 binary 指针与长度配对. */
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
          /* PCM 和普通文本是可丢弃的; 关闭/失败控制事件必须保留,
             否则 FreeSWITCH 侧可能永远持有 AudioPipe owner 引用. */
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
          /* 队列里只剩控制事件时, 允许当前控制事件入队, 优先保证
             CONNECT_FAIL / CONNECTION_CLOSED 等终止通知不被静默丢弃. */
        }
        event_queue.emplace_back(std::move(pending));
      }
      event_cv.notify_one();
    } catch (...) {
      /* 回调位于 LWS 服务线程, 不能让分配异常穿透 libwebsockets. */
      pipe->release();
    }
  }

  /**
   * @brief 初始化单个通话的私有数据结构体 (private_t)
   *
   * 包含以下核心初始化步骤:
   * 1. 参数与合法性校验: 检查采样率 (8k~64k 且为 8000 倍数)、声道数 (1 或 2)、端口范围及 URL 路径长度;
   * 2. 读取通道基本认证配置 (MOD_AUDIO_BASIC_AUTH_*);
   * 3. 填充基础字段并初始化原子状态 (buffer_overrun_notified, audio_paused 等);
   * 4. 计算上行音频环形缓冲区容量并实例化 drachtio::AudioPipe;
   * 5. 初始化互斥锁与下行 WebSocket 内存桥环形缓冲 (默认最大 2 MiB);
   * 6. 当通道物理采样率与期望采样率不一致时, 初始化 Speex 重采样器.
   *
   * @param tech_pvt 会话私有数据指针
   * @param session FreeSWITCH 会话指针
   * @param host 目标 WebSocket 主机名或 IP
   * @param port 目标 WebSocket 端口
   * @param path 目标 WebSocket 请求路径
   * @param sslFlags SSL/TLS 配置标志 (如 LCCSCF_USE_SSL)
   * @param sampling 通道物理采样率
   * @param desiredSampling 上游期望的采样率
   * @param channels 声道数 (1=单声道, 2=双声道)
   * @param bugname 关联的 Media Bug 标识名
   * @param metadata 初始元数据 JSON 字符串 (可选)
   * @param responseHandler 业务事件通知回调
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回错误码
   */
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

    /* 初始化下行 WebSocket 内存桥环形缓冲区与互斥锁(最大 2 MiB, 覆盖 5 秒 64kHz 双声道预缓冲) */
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
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "初始化重采样器时出错: %s.\n", speex_resampler_strerror(err));
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

  /**
   * @brief 彻底销毁会话私有数据 (private_t) 中分配的各项资源
   *
   * 清理流程:
   * 1. 释放并销毁关联的 drachtio::AudioPipe (关闭底层 WebSocket 连接);
   * 2. 销毁 Speex 重采样器实例;
   * 3. 停止接收下行音频并将 interrupted 设为 1;
   * 4. 最长等待 2 秒直到下行活跃播放句柄计数归零 (downstream_handles == 0);
   * 5. 安全销毁下行环形动态缓冲区 (downstream_buffer);
   * 6. 将生命周期状态标记为 AUDIO_FORK_LIFECYCLE_CLOSED.
   *
   * @param tech_pvt 会话私有数据指针
   */
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
  /**
   * @brief C 导出接口: 销毁会话私有数据
   *
   * @param tech_pvt 会话私有数据指针
   */
  void fork_data_destroy(private_t *tech_pvt) {
    destroy_tech_pvt(tech_pvt);
  }

  /**
   * @brief 解析 WebSocket 目标 URI 字符串
   *
   * 解析并校验:
   * 1. 协议协议头 (ws://, wss://, http://, https://), 并按协议设置默认端口与 SSL 标志;
   * 2. 提取 host 与可选 port, 支持 IPv6 括号地址格式;
   * 3. 提取 path, 若路径为空则补齐为 "/";
   * 4. 读取通道变量配置 TLS 宽松参数: 自签名允许、跳过主机名校验、允许证书过期.
   *
   * @param channel FreeSWITCH 通道指针 (用于读取 TLS 配置通道变量)
   * @param szServerUri 完整的 WebSocket URI 字符串
   * @param host 接收主机名缓冲 (最长 MAX_WS_URL_LEN)
   * @param path 接收请求路径缓冲 (最长 MAX_PATH_LEN)
   * @param pPort 接收端口号指针
   * @param pSslFlags 接收 SSL 标志位指针
   * @return int 成功返回 1, 格式非法或超出长度返回 0
   */
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
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无效的协议: %s, 必须是 ws/wss 或 http/https\n", scheme.c_str());
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

  /**
   * @brief libwebsockets 内部日志输出转接函数
   *
   * @param level libwebsockets 日志等级 (LLL_ERR, LLL_WARN, LLL_NOTICE, LLL_INFO 等)
   * @param line 格式化后的日志行文本
   */
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

  /**
   * @brief 模块全局初始化
   *
   * 启动步骤:
   * 1. 输出缓冲区配置和子协议名称日志;
   * 2. 启动异步事件工作线程 (startEventWorker);
   * 3. 初始化 drachtio::AudioPipe 底层 LWS 上下文及网络服务线程池.
   *
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回 SWITCH_STATUS_FALSE
   */
  switch_status_t fork_init(void) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 音频缓冲(秒):    %d 秒\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 子协议:              %s\n", mySubProtocolName);

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

  /**
   * @brief 模块全局卸载清理
   *
   * 清理步骤:
   * 1. 停止事件分发工作线程 (stopEventWorker), 避免卸载期间再向 FreeSWITCH 投递事件;
   * 2. 调用 AudioPipe::deinitialize() 关闭所有网络连接与线程池;
   * 3. 返回卸载状态.
   *
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回 SWITCH_STATUS_FALSE
   */
  switch_status_t fork_cleanup(void) {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 正在卸载..\n");
    /* 模块卸载时先停止业务事件线程, 避免 LWS 收尾回调在模块代码卸载
       期间重新进入 FreeSWITCH; deinitialize 随后释放所有 pipe 引用. */
    stopEventWorker();
    cleanup = drachtio::AudioPipe::deinitialize();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 卸载状态 %d\n", cleanup);
    if (cleanup == true) {
      return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  /**
   * @brief 驱动服务线程工作 (在独立线程模型下保持桩函数)
   *
   * @return switch_status_t 恒返回 SWITCH_STATUS_SUCCESS
   */
  switch_status_t fork_service_threads(void) {
    return SWITCH_STATUS_SUCCESS;
  }

  /**
   * @brief 为单个通话会话初始化音频分叉私有数据与管道
   *
   * @param session FreeSWITCH 通话核心会话
   * @param responseHandler 业务事件通知回调
   * @param samples_per_second 通道原生物理采样率 (如 8000, 16000)
   * @param host 目标 WebSocket 主机名或 IP
   * @param port 目标 WebSocket 端口
   * @param path 目标 WebSocket 请求路径
   * @param sampling 期望采样的输出采样率 (如 16000)
   * @param sslFlags SSL/TLS 配置标志
   * @param channels 声道数 (1=单声道, 2=双声道)
   * @param bugname 关联的 Media Bug 名称
   * @param metadata 初始元数据 JSON 文本 (握手成功后自动发送)
   * @param ppUserData 输出分配的 private_t 私有数据指针
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回 SWITCH_STATUS_FALSE
   */
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
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配内存出错!\n");
      return SWITCH_STATUS_FALSE;
    }
    /* fork_data_init 可能在读取 codec 前失败, 先清零以保证失败收尾安全. */
    memset(tech_pvt, 0, sizeof(*tech_pvt));

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, metadata, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

  /**
   * @brief 异步发起 WebSocket 连接
   *
   * @param ppUserData 传入 private_t 指针的地址
   * @return switch_status_t 连接发起成功返回 SWITCH_STATUS_SUCCESS, 失败返回 SWITCH_STATUS_FALSE
   */
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

  /**
   * @brief 清理并关闭通话会话的音频分叉管道
   *
   * 清理流程:
   * 1. 检查 lifecycle_state 是否处于 ACTIVE 且 cleanup_started 未置位, 避免重复清理;
   * 2. 状态原子切换为 CLOSING 并置 cleanup_started=1;
   * 3. 从通道 private 变量中注销 bugname;
   * 4. 若传入尾部控制文本 (text), 将其压入 AudioPipe 发送队列;
   * 5. 调用 closeAndDestroy 关闭 WebSocket;
   * 6. 若通话通道未处于挂断流程 (channelIsClosing=0), 显式调用 switch_core_media_bug_remove;
   * 7. 调用 destroy_tech_pvt 释放全部底层资源, 状态更新为 CLOSED.
   *
   * @param session FreeSWITCH 会话
   * @param bug 关联的 Media Bug
   * @param text 关闭前发送的可选文本消息
   * @param channelIsClosing 通道是否正在挂断 (若是, 则由 FreeSWITCH 核心销毁 media bug)
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
   */
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
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%u) fork_session_cleanup: 连接已关闭\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

  /**
   * @brief 向 WebSocket 连接对端发送文本消息 (JSON)
   *
   * @param session FreeSWITCH 会话
   * @param bugname 关联的 Media Bug 名称
   * @param text 待发送的文本字符串
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 通道未找到或管道无效返回 SWITCH_STATUS_FALSE
   */
  switch_status_t fork_session_send_text(switch_core_session_t *session, char *bugname, char* text) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_send_text 失败, 因为没有bug\n");
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

  /**
   * @brief 暂停或恢复通话音频分叉的数据推送
   *
   * @param session FreeSWITCH 会话
   * @param bugname 关联的 Media Bug 名称
   * @param pause 1 表示暂停推送, 0 表示恢复推送
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 通道未找到返回 SWITCH_STATUS_FALSE
   */
  switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_pauseresume 失败, 因为没有bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    switch_atomic_set(&tech_pvt->audio_paused, pause ? 1U : 0U);
    return SWITCH_STATUS_SUCCESS;
  }

  /**
   * @brief 触发 WebSocket 优雅断开 (发送完积压数据后再关闭连接)
   *
   * @param session FreeSWITCH 会话
   * @param bugname 关联的 Media Bug 名称
   * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 通道未找到返回 SWITCH_STATUS_FALSE
   */
  switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_graceful_shutdown 失败, 因为没有bug\n");
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

  /**
   * @brief Media Bug 音频采集回调核心入口 (SWITCH_ABC_TYPE_READ)
   *
   * 核心逻辑:
   * 1. 检查暂停与清理状态, 若已暂停或正在清理则直接返回 SWITCH_TRUE (不消费音频);
   * 2. trylock 尝试获取会话互斥锁, 避免阻塞 FreeSWITCH 核心混音时钟;
   * 3. 检查 AudioPipe 连接状态 (必须为 LWS_CLIENT_CONNECTED);
   * 4. 检查上行缓冲区空间: 若剩余空间低于 binaryMinSpace() 门限, 直接跳过避免溢出;
   * 5. 音频写入:
   *    - 若无重采样需求 (sampling == desiredSampling): 直通写入;
   *    - 若需要重采样 (例如 8kHz -> 16kHz): 调用 speex_resampler_process_interleaved_int 转换后写入;
   * 6. 溢出保护: 写入过程中若空间不足, 调用 notify_upstream_buffer_overrun 记录日志并通知 ESL 业务层.
   */
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

      /* 1. 直通写入分支 (无需重采样) */
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
              notify_upstream_buffer_overrun(session, tech_pvt);
              break;
            }
            memset(&frame, 0, sizeof(frame));
            frame.data = data;
            frame.buflen = sizeof(data);
          }
        }
      }
      /* 2. Speex 重采样写入分支 */
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
              notify_upstream_buffer_overrun(session, tech_pvt);
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
              notify_upstream_buffer_overrun(session, tech_pvt);
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
