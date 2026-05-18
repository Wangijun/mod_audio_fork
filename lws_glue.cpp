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
#include <fstream>
#include <sstream>
#include <regex>

#include "base64.hpp"
#include "parser.hpp"
#include "mod_audio_fork.h"
#include "audio_pipe.hpp"
#include "vector_math.h"

#include <boost/circular_buffer.hpp>


typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*这意味着在8kHz下每个20ms帧为320字节（仅单声道）*/
#define BUFFER_GROW_SIZE (16384)
#define MAX_MARKS (30)
#define MAX_DUB_FRAME_SAMPLES 2048
#define MAX_CIRCULAR_BUFFER_CAPACITY (1024 * 1024)

namespace {
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads __attribute__((unused)) = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static std::atomic<unsigned int> idxCallCount{0};

  static bool markCountExceeded(private_t* tech_pvt) {
    if (nullptr != tech_pvt->pVecMarksInUse) {
      std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
      std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      return pVecMarksInUse->size()+ pVecMarksInInventory->size() >= MAX_MARKS;
    }
    return false;
  }

  switch_status_t processIncomingBinary(private_t* tech_pvt, switch_core_session_t* /*session*/, const char* message, size_t dataLength) {
    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    std::vector<uint8_t> data;

    // 如果存在预留字节，则前置它
    if (tech_pvt->has_set_aside_byte) {
        data.push_back(tech_pvt->set_aside_byte);
        tech_pvt->has_set_aside_byte = false;
    }

    // 追加新传入的消息
    data.insert(data.end(), message, message + dataLength);

    // 检查总数据长度是否为奇数
    if (data.size() % 2 != 0) {
        // 将最后一个字节预留
        tech_pvt->set_aside_byte = data.back();
        tech_pvt->has_set_aside_byte = true;
        data.pop_back(); // 从数据向量中移除最后一个字节
    }

    // 将数据转换为16位元素
    const uint16_t* data_uint16 = reinterpret_cast<const uint16_t*>(data.data());
    size_t numSamples = data.size() / sizeof(uint16_t);

    // 访问预缓冲区
    CircularBuffer_t* cBuffer = static_cast<CircularBuffer_t*>(tech_pvt->streamingPreBuffer);

    std::deque<std::string>* pVecMarksInInventory = nullptr;
    std::deque<std::string>* pVecMarksInUse = nullptr;
    if (nullptr != tech_pvt->pVecMarksInInventory) {
      pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);

      // 记录标记位置到带外数据结构
      if (tech_pvt->pMarkerPositions == nullptr) {
        tech_pvt->pMarkerPositions = new std::vector<std::pair<size_t, std::string>>();
      }
      auto* markers = static_cast<std::vector<std::pair<size_t, std::string>>*>(tech_pvt->pMarkerPositions);
      for (auto& markName : *pVecMarksInInventory) {
        markers->push_back({cBuffer->size(), markName});
      }

      // 将库存移至使用中
      pVecMarksInUse->insert(pVecMarksInUse->end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());
      pVecMarksInInventory->clear();
    }

    // 确保预缓冲区有足够的容量
    if (cBuffer->capacity() - cBuffer->size() < numSamples) {
        size_t newCapacity = cBuffer->size() + std::max(numSamples, (size_t)BUFFER_GROW_SIZE);
        if (newCapacity > MAX_CIRCULAR_BUFFER_CAPACITY) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "(%u) pre-buffer 超过最大容量，清空\n", tech_pvt->id);
            cBuffer->clear();
            newCapacity = std::min((size_t)MAX_CIRCULAR_BUFFER_CAPACITY, cBuffer->size() + numSamples);
        }
        cBuffer->set_capacity(newCapacity);
    }

    // 将数据追加到预缓冲区
    cBuffer->insert(cBuffer->end(), data_uint16, data_uint16 + numSamples);
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "已追加 %zu 个16位采样到预缓冲区。\n", numSamples);

    // 如果预缓冲数据量未达到阈值，则返回
    if ((int)cBuffer->size() < tech_pvt->streamingPreBufSize) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "预缓冲数据低于阈值 %u，返回。\n", tech_pvt->streamingPreBufSize);
        if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_STATUS_SUCCESS;
    }

    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "预缓冲数据采样数 %u 已超过阈值 %u，准备播放。\n", cBuffer->size(), tech_pvt->streamingPreBufSize);

    // tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 2; // 已移除动态调整大小

    // 检查降采样因子
    size_t downsample_factor = tech_pvt->downscale_factor;

    // 计算可以被降采样因子整除的采样数
    size_t numCompleteSamples = (cBuffer->size() / downsample_factor) * downsample_factor;

    // 处理剩余采样
    std::vector<uint16_t> leftoverSamples;
    size_t numLeftoverSamples = cBuffer->size() - numCompleteSamples;
    if (numLeftoverSamples > 0) {
        leftoverSamples.assign(cBuffer->end() - numLeftoverSamples, cBuffer->end());
        cBuffer->resize(numCompleteSamples);
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "由于降采样，临时移除 %u 个剩余采样。\n", numLeftoverSamples);
    }

    // 必要时重采样
    std::vector<int16_t> out;
    try {
      if (tech_pvt->bidirectional_audio_resampler) {
        // 改进：使用assign将循环缓冲区转换为向量以进行重采样
        std::vector<int16_t> in;
        in.assign(cBuffer->begin(), cBuffer->end());
        out.resize(in.size() * 8); // 最大上采样为 8k -> 48k

        spx_uint32_t in_len = in.size();
        spx_uint32_t out_len = out.size();

        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "重采样 %u 个采样到可容纳 %u 个采样的缓冲区\n", in.size(), out_len);

        speex_resampler_process_interleaved_int(tech_pvt->bidirectional_audio_resampler, in.data(), &in_len, out.data(), &out_len);

        // 调整输出缓冲区大小以匹配重采样器的输出长度
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "将输出缓冲区从 %u 调整为 %u 个采样\n", in.size(), out_len);

        out.resize(out_len);
      }
      else {
        // 如果不需要重采样，将预缓冲区的数据复制到输出缓冲区
        out.assign(cBuffer->begin(), cBuffer->end());
      }
    } catch (const std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "重采样传入二进制消息时出错：%s\n", e.what());
      if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
      return SWITCH_STATUS_FALSE;
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "重采样传入二进制消息时出错\n");
      if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
      return SWITCH_STATUS_FALSE;
    }

    {
      CircularBuffer_t *playoutBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

      try {
        // 必要时调整缓冲区大小
        if (playoutBuffer->capacity() - playoutBuffer->size() < out.size()) {
          size_t newCapacity = playoutBuffer->size() + std::max(out.size(), (size_t)BUFFER_GROW_SIZE);
          if (newCapacity > MAX_CIRCULAR_BUFFER_CAPACITY) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                  "(%u) playout buffer 超过最大容量，清空\n", tech_pvt->id);
              playoutBuffer->clear();
              newCapacity = std::min((size_t)MAX_CIRCULAR_BUFFER_CAPACITY, playoutBuffer->size() + out.size());
          }
          playoutBuffer->set_capacity(newCapacity);
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "已将播放缓冲区调整为新容量：%zu\n", newCapacity);
        }
        // 将数据推入缓冲区
        playoutBuffer->insert(playoutBuffer->end(), out.begin(), out.end());
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "已追加 %zu 个16位采样到播放缓冲区。\n", out.size());
      } catch (const std::exception& e) {
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "处理传入二进制消息时出错：%s\n", e.what());
        if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_STATUS_FALSE;
      } catch (...) {
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "处理传入二进制消息时出错\n");
        if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_STATUS_FALSE;
      }
      cBuffer->clear();

      // 将剩余采样放回预缓冲区以备下次使用
      if (!leftoverSamples.empty()) {
          cBuffer->insert(cBuffer->end(), leftoverSamples.begin(), leftoverSamples.end());
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "将 %u 个剩余采样放回预缓冲区。\n", leftoverSamples.size());
      }
      if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
      return SWITCH_STATUS_SUCCESS;
    }
  }

  void processIncomingMessage(private_t* tech_pvt, switch_core_session_t* session, const char* message) {
    std::string msg = message;
    std::string type;
    cJSON* json = parse_json(session, msg, type) ;
    if (json) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - 收到 %s 消息 %s\n", tech_pvt->id, type.c_str(), message);
      cJSON* jsonData = cJSON_GetObjectItem(json, "data");
      if (0 == type.compare("playAudio") &&
        // 已启用playAudio且未启用流的双向音频
        tech_pvt->bidirectional_audio_enable &&
        !tech_pvt->bidirectional_audio_stream) {
        if (jsonData) {
          cJSON* jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioContent");
          int validAudio = (jsonAudio && NULL != jsonAudio->valuestring);
          const char* szAudioContentType = cJSON_GetObjectCstr(jsonData, "audioContentType");

          if (!validAudio) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - playAudio请求中缺少audioContent\n", tech_pvt->id);
          }
          else if (!szAudioContentType || 0 != strcmp(szAudioContentType, "raw")) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "(%u) processIncomingMessage - 不支持的audioContentType：%s。实时流需要raw格式。\n", tech_pvt->id, szAudioContentType);
          }
          else {
            // 解码Base64
            std::string rawAudio = drachtio::base64_decode(jsonAudio->valuestring);
            size_t decodedLen = rawAudio.length();
            uint8_t *data = (uint8_t *) rawAudio.data();

            // 获取采样率（当前未使用，但已解析以备将来使用）
            cJSON* jsonSR = cJSON_GetObjectItem(jsonData, "sampleRate");
            (void)jsonSR;

            // 访问缓冲区 - 使用阻塞锁以确保数据完整性
            if (nullptr != tech_pvt->mutex && switch_mutex_lock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
                CircularBuffer_t *playoutBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
                try {
                    // 将原始音频转换为int16_t向量
                    const int16_t* pAudio = reinterpret_cast<const int16_t*>(data);
                    size_t numSamples = decodedLen / sizeof(int16_t);

                    // 必要时调整缓冲区大小
                    if (playoutBuffer->capacity() - playoutBuffer->size() < numSamples) {
                        size_t newCapacity = playoutBuffer->size() + std::max(numSamples, (size_t)BUFFER_GROW_SIZE);
                        if (newCapacity > MAX_CIRCULAR_BUFFER_CAPACITY) {
                            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                "(%u) playout buffer 超过最大容量，清空\n", tech_pvt->id);
                            playoutBuffer->clear();
                            newCapacity = std::min((size_t)MAX_CIRCULAR_BUFFER_CAPACITY, playoutBuffer->size() + numSamples);
                        }
                        playoutBuffer->set_capacity(newCapacity);
                    }
                    // 将数据推入缓冲区
                    playoutBuffer->insert(playoutBuffer->end(), pAudio, pAudio + numSamples);

                    // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) 已缓冲 %zu 字节TTS音频。缓冲区大小：%zu\n", tech_pvt->id, decodedLen, playoutBuffer->size());

                } catch (const std::exception& e) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "缓冲TTS音频时出错：%s\n", e.what());
                } catch (...) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "缓冲TTS音频时出错\n");
                }

                switch_mutex_unlock(tech_pvt->mutex);
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) 为缓冲TTS锁定互斥锁失败\n", tech_pvt->id);
            }
          }
          if (jsonAudio) cJSON_Delete(jsonAudio);
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - playAudio请求中缺少data负载\n", tech_pvt->id);
        }
      }
      else if (0 == type.compare("killAudio")) {
        tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, NULL);

        // 终止通道上当前的播放
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_channel_set_flag_value(channel, CF_BREAK, 2);

        // 这将丢弃已缓冲的传入音频
        if (tech_pvt->mutex) {
            switch_mutex_lock(tech_pvt->mutex);
            tech_pvt->clear_bidirectional_audio_buffer = true;
            switch_mutex_unlock(tech_pvt->mutex);
        }
      }
      else if (0 == type.compare("mark")) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        if (data) {
          cJSON* name = cJSON_GetObjectItem(data, "name");
          if (cJSON_IsString(name) && name->valuestring) {
            if (markCountExceeded(tech_pvt)) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%u) processIncomingMessage - 标记数量超限，丢弃标记 %s\n", tech_pvt->id, cJSON_GetStringValue(name));
            }
            else {
              if (nullptr == tech_pvt->pVecMarksInInventory) {
                tech_pvt->pVecMarksInInventory = static_cast<void *>(new std::deque<std::string>());
                tech_pvt->pVecMarksInUse = static_cast<void *>(new std::deque<std::string>());
                tech_pvt->pVecMarksCleared = static_cast<void *>(new std::deque<std::string>());
              }
              std::deque<std::string>* pVec = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
              if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
              pVec->push_back(name->valuestring);
              if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
            }
          }
        }
      }
      else if (0 == type.compare("clearMarks")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - 收到clearMarks\n", tech_pvt->id);
        if (nullptr != tech_pvt->pVecMarksInInventory) {
          if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
          std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
          std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
          std::deque<std::string>* pVecMarksCleared = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
          pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInUse->begin(), pVecMarksInUse->end());
          pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());
          pVecMarksInInventory->clear();
          pVecMarksInUse->clear();
          if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
        }
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
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - 不支持的消息类型 %s\n", tech_pvt->id, type.c_str());
      }
      cJSON_Delete(json);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - 无法解析消息：%s\n", tech_pvt->id, message);
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
          switch (event) {
            case drachtio::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "连接成功\n");
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
              if (strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "发送初始元数据 %s\n", tech_pvt->initialMetadata);
                if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
                drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
                if (pAudioPipe) pAudioPipe->bufferForSending(tech_pvt->initialMetadata);
                if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
              }
            break;
            case drachtio::AudioPipe::CONNECT_FAIL:
            {
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              if (tech_pvt->mutex) {
                switch_mutex_lock(tech_pvt->mutex);
                tech_pvt->pAudioPipe = nullptr;
                switch_mutex_unlock(tech_pvt->mutex);
              } else {
                tech_pvt->pAudioPipe = nullptr;
              }
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) json.str().c_str());
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
    char *bugname, char* metadata, int bidirectional_audio_enable,
    int bidirectional_audio_stream, int bidirectional_audio_sample_rate, responseHandler_t responseHandler) {

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    int bidirectional_audio_stream_enable = bidirectional_audio_enable + bidirectional_audio_stream;
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
    tech_pvt->streamingPlayoutBuffer = (void *) new CircularBuffer_t(8192);
    tech_pvt->bidirectional_audio_enable = bidirectional_audio_enable;
    tech_pvt->bidirectional_audio_stream = bidirectional_audio_stream;
    tech_pvt->bidirectional_audio_sample_rate = bidirectional_audio_sample_rate;
    tech_pvt->clear_bidirectional_audio_buffer = false;
    tech_pvt->has_set_aside_byte = 0;
    tech_pvt->downscale_factor = 1;
    tech_pvt->raw_write_codec_initialized = 0;
    tech_pvt->write_ts = 0;
    if (bidirectional_audio_sample_rate > sampling) {
      tech_pvt->downscale_factor = bidirectional_audio_sample_rate / sampling;
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "降采样因子为 %d\n", tech_pvt->downscale_factor);
    }
    tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 6; // 最小120ms预缓冲
    tech_pvt->streamingPreBuffer = (void *) new CircularBuffer_t(8192);
    tech_pvt->pVecMarksInInventory = nullptr;
    tech_pvt->pVecMarksInUse = nullptr;
    tech_pvt->pVecMarksCleared = nullptr;
    tech_pvt->pMarkerPositions = nullptr;

    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);
    tech_pvt->bugname[MAX_BUG_LEN] = '\0';
    if (metadata) {
      strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN - 1);
      tech_pvt->initialMetadata[MAX_METADATA_LEN - 1] = '\0';
    }

    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    drachtio::AudioPipe* ap = new drachtio::AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags,
      buflen, read_impl.decoded_bytes_per_packet, username, password, bugname, bidirectional_audio_stream_enable, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配AudioPipe时出错\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

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

    if (bidirectional_audio_sample_rate && sampling != bidirectional_audio_sample_rate) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) 双向音频从 %u 重采样到 %u，声道数 %d\n", tech_pvt->id, bidirectional_audio_sample_rate, sampling, channels);
      tech_pvt->bidirectional_audio_resampler = speex_resampler_init(1, bidirectional_audio_sample_rate, sampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "初始化双向音频重采样器时出错：%s。\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void destroy_tech_pvt(private_t* tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->bidirectional_audio_resampler) {
      speex_resampler_destroy(tech_pvt->bidirectional_audio_resampler);
      tech_pvt->bidirectional_audio_resampler = nullptr;
    }
    if (tech_pvt->raw_write_codec_initialized) {
      switch_core_codec_destroy(&tech_pvt->raw_write_codec);
      tech_pvt->raw_write_codec_initialized = 0;
    }
    if (tech_pvt->mutex) {
      switch_mutex_destroy(tech_pvt->mutex);
      tech_pvt->mutex = nullptr;
    }
    if (tech_pvt->streamingPlayoutBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
      delete cBuffer;
      tech_pvt->streamingPlayoutBuffer = nullptr;
    }
    if (tech_pvt->streamingPreBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPreBuffer;
      delete cBuffer;
      tech_pvt->streamingPreBuffer = nullptr;
    }

    if (tech_pvt->pVecMarksInInventory) {
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      tech_pvt->pVecMarksInInventory = nullptr;
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
      tech_pvt->pVecMarksInUse = nullptr;
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
      tech_pvt->pVecMarksCleared = nullptr;
    }
    if (tech_pvt->pMarkerPositions) {
      delete static_cast<std::vector<std::pair<size_t, std::string>>*>(tech_pvt->pMarkerPositions);
      tech_pvt->pMarkerPositions = nullptr;
    }
    if (tech_pvt->pAudioPipe) {
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      drachtio::AudioPipe::removeFromPendingLists(pAudioPipe);
      delete pAudioPipe;
      tech_pvt->pAudioPipe = nullptr;
    }
  }

  static void send_mark_event(private_t* tech_pvt, const char* name, int cleared = false) {
    std::ostringstream json;
    json << "{\"type\": \"mark\", \"data\": {\"name\":\"" << name << "\", ";
    if (cleared) json << "\"event\": \"cleared\"}}";
    else json << "\"event\": \"playout\"}}";

    if (tech_pvt->mutex) switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) {
      std::string str = json.str();
      pAudioPipe->bufferForSending(str.c_str());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) send_mark_event：%s\n", tech_pvt->id, str.c_str());
    }
    if (tech_pvt->mutex) switch_mutex_unlock(tech_pvt->mutex);
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
    break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }
}

extern "C" {
  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    int offset;
    char server[MAX_WS_URL_LEN + MAX_PATH_LEN];
    int flags = LCCSCF_USE_SSL;

    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - 允许自签名证书\n");
      flags |= LCCSCF_ALLOW_SELFSIGNED;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - 跳过主机名检查\n");
      flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - 允许过期证书\n");
      flags |= LCCSCF_ALLOW_EXPIRED;
    }

    // 获取协议方案
    strncpy(server, szServerUri, MAX_WS_URL_LEN + MAX_PATH_LEN - 1);
    server[MAX_WS_URL_LEN + MAX_PATH_LEN - 1] = '\0';
    if (0 == strncmp(server, "https://", 8) || 0 == strncmp(server, "HTTPS://", 8)) {
      *pSslFlags = flags;
      offset = 8;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "wss://", 6) || 0 == strncmp(server, "WSS://", 6)) {
      *pSslFlags = flags;
      offset = 6;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "http://", 7) || 0 == strncmp(server, "HTTP://", 7)) {
      offset = 7;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else if (0 == strncmp(server, "ws://", 5) || 0 == strncmp(server, "WS://", 5)) {
      offset = 5;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - 解析uri %s出错：无效的协议方案\n", szServerUri);;
      return 0;
    }

    std::string strHost(server + offset);
    //- `([^/:]+)` 捕获主机名/IP地址，匹配除集合内字符外的任意字符
    //- `:?([0-9]*)?` 可选地捕获冒号和端口号（如果存在）
    //- `(/.*)` 捕获其余所有内容（路径）
    std::regex re("([^/:]+):?([0-9]*)?(/.*)?$");
    std::smatch matches;
    if(std::regex_search(strHost, matches, re)) {
      /*
      for (int i = 0; i < matches.length(); i++) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - %d: %s\n", i, matches[i].str().c_str());
      }
      */
      strncpy(host, matches[1].str().c_str(), MAX_WS_URL_LEN);
      if (matches[2].str().length() > 0) {
        *pPort = atoi(matches[2].str().c_str());
      }
      if (matches[3].str().length() > 0) {
        strncpy(path, matches[3].str().c_str(), MAX_PATH_LEN);
      }
      else {
        strcpy(path, "/");
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - 无效格式 %s\n", strHost.c_str());
      return 0;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - 主机 %s，路径 %s\n", host, path);

    return 1;
  }

  switch_status_t fork_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 音频缓冲（秒）：    %d 秒\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: 子协议：              %s\n", mySubProtocolName);

    //int logs = LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    drachtio::AudioPipe::initialize(mySubProtocolName, logs, lws_logger);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 初始化成功\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup() {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 正在卸载..\n");

    cleanup = drachtio::AudioPipe::deinitialize();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork 卸载状态 %d\n", cleanup);
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
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
    int bidirectional_audio_enable,
    int bidirectional_audio_stream,
    int bidirectional_audio_sample_rate,
    void **ppUserData
    )
  {
    // 分配每会话数据结构
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "分配内存出错！\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, metadata, bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate, responseHandler)) {
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

    // 在加锁后重新获取bug
    {
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        switch_channel_set_private(channel, bugname, NULL);
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }
      }
    }

    // 删除所有临时文件
    struct playout* playout = tech_pvt->playout;
    while (playout) {
      std::remove(playout->file);
      free(playout->file);
      struct playout *tmp = playout;
      playout = playout->next;
      free(tmp);
    }

    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (pAudioPipe) pAudioPipe->close();
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

    if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->graceful_shutdown) return SWITCH_TRUE;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
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
      size_t available = pAudioPipe->binarySpaceAvailable();
      if (NULL == tech_pvt->resampler) {
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (true) {

          // 检查缓冲区是否会被覆盖；如果是则丢弃数据包
          if (available < pAudioPipe->binaryMinSpace()) {
            if (!tech_pvt->buffer_overrun_notified) {
              tech_pvt->buffer_overrun_notified = 1;
              tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) 正在丢弃数据包！\n",
              tech_pvt->id);
            pAudioPipe->binaryWritePtrReset();

            frame.data = pAudioPipe->binaryWritePtr();
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
          }

          switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
          if (rv != SWITCH_STATUS_SUCCESS) break;
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
            spx_uint32_t out_len = available >> 1;  // 每个2字节采样的空间
            spx_uint32_t in_len = frame.samples;

            speex_resampler_process_interleaved_int(tech_pvt->resampler,
              (const spx_int16_t *) frame.data,
              (spx_uint32_t *) &in_len,
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              // 写入字节数 = 采样数 * 2 * 声道数
              size_t bytes_written = out_len << tech_pvt->channels;
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

switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t* tech_pvt) {
    if (!tech_pvt) return SWITCH_TRUE;

    static std::atomic<uint32_t> call_count{0};
    static std::atomic<uint32_t> underrun_count{0};
    call_count.fetch_add(1, std::memory_order_relaxed);

    // 使用阻塞锁 - 对同步至关重要
    switch_mutex_lock(tech_pvt->mutex);

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

    if ((call_count.load(std::memory_order_relaxed) % 50) == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                         "(%u) 系统活跃 #%u：缓冲区=%zu 采样，欠载=%u\n",
                         tech_pvt->id, call_count.load(std::memory_order_relaxed), cBuffer->size(), underrun_count.load(std::memory_order_relaxed));
    }

    // 清空缓冲区逻辑
    if (tech_pvt->clear_bidirectional_audio_buffer) {
        cBuffer->clear();
        tech_pvt->clear_bidirectional_audio_buffer = false;

        // 处理已清除的标记
        if (nullptr != tech_pvt->pVecMarksInInventory) {
            std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
            std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
            std::deque<std::string>* pVecMarksCleared = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);

            if (pVecMarksInInventory->size() + pVecMarksInUse->size() > 0) {
                std::deque<std::string> vec = *pVecMarksInUse;
                vec.insert(vec.end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());

                for (auto it = vec.begin(); it != vec.end(); ++it) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                                     "(%u) 标记 %s 已清除\n", tech_pvt->id, it->c_str());
                    send_mark_event(tech_pvt, it->c_str(), true);
                }

                pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInUse->begin(), pVecMarksInUse->end());
                pVecMarksInUse->clear();
                pVecMarksInInventory->clear();
            }
        }
    } else {
        // 从FreeSWITCH获取帧
        switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);

        if (rframe && rframe->datalen > 0) {
            int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);
            int samples_needed = rframe->samples;

            // 检查可用数据
            int samplesToCopy = std::min(static_cast<int>(cBuffer->size()), samples_needed);

            if (samplesToCopy > 0) {
                // 用静音准备数据
                int16_t data[MAX_DUB_FRAME_SAMPLES];
                if (samples_needed > MAX_DUB_FRAME_SAMPLES) {
                    switch_mutex_unlock(tech_pvt->mutex);
                    return SWITCH_TRUE;
                }
                memset(data, 0, samples_needed * sizeof(int16_t));
                std::copy_n(cBuffer->begin(), samplesToCopy, data);
                cBuffer->erase_begin(samplesToCopy);

                // 处理带外标记
                if (tech_pvt->pMarkerPositions) {
                    auto* markers = static_cast<std::vector<std::pair<size_t, std::string>>*>(tech_pvt->pMarkerPositions);
                    for (auto it = markers->begin(); it != markers->end(); ) {
                        if (it->first < (size_t)samplesToCopy) {
                            send_mark_event(tech_pvt, it->second.c_str(), false);
                            it = markers->erase(it);
                        } else {
                            it->first -= samplesToCopy;
                            ++it;
                        }
                    }
                }

                // 完全替换原始帧
                memcpy(fp, data, samples_needed * sizeof(int16_t));
                rframe->channels = 1;
                rframe->datalen = samples_needed * sizeof(int16_t);

                // 应用替换
                switch_core_media_bug_set_write_replace_frame(bug, rframe);

            } else {
                underrun_count.fetch_add(1, std::memory_order_relaxed);
                // 空缓冲区：发送静音以避免断音
                memset(fp, 0, samples_needed * sizeof(int16_t));
                rframe->channels = 1;
                rframe->datalen = samples_needed * sizeof(int16_t);
                switch_core_media_bug_set_write_replace_frame(bug, rframe);
            }
        }
    }

    switch_mutex_unlock(tech_pvt->mutex);
    return SWITCH_TRUE;
}

  switch_status_t fork_session_stop_play(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_stop_play 失败，因为没有bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    if (switch_mutex_lock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
      if (cBuffer != nullptr) {
        cBuffer->clear();
      }
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_STATUS_SUCCESS;
  }

}
