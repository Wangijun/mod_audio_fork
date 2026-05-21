#include "audio_pipe.hpp"
#include <switch.h>

#include <cassert>
#include <iostream>

/* 丢弃超过此长度的套接字传入文本消息 */
#define MAX_RECV_BUF_SIZE (65 * 1024 * 10)
#define RECV_BUF_REALLOC_SIZE (8 * 1024)

using namespace drachtio;

namespace {
  static const char* basicAuthUser = std::getenv("MOD_AUDIO_FORK_HTTP_AUTH_USER");
  static const char* basicAuthPassword = std::getenv("MOD_AUDIO_FORK_HTTP_AUTH_PASSWORD");

  static const char *requestedTcpKeepaliveSecs = std::getenv("MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS");
  static int nTcpKeepaliveSecs = requestedTcpKeepaliveSecs ? ::atoi(requestedTcpKeepaliveSecs) : 55;
}

// 更新到包含此辅助函数的 lws 版本后删除
static int dch_lws_http_basic_auth_gen(const char *user, const char *pw, char *buf, size_t len) {
	size_t n = strlen(user), m = strlen(pw);
	char b[128];

	if (len < 6 + ((4 * (n + m + 1)) / 3) + 1)
		return 1;

	memcpy(buf, "Basic ", 6);

	n = lws_snprintf(b, sizeof(b), "%s:%s", user, pw);
	if (n >= sizeof(b) - 2)
		return 2;

	lws_b64_encode_string(b, n, buf + 6, len - 6);
	buf[len - 1] = '\0';

	return 0;
}

int AudioPipe::lws_callback(struct lws *wsi,
  enum lws_callback_reasons reason,
  void *user, void *in, size_t len) {

  struct AudioPipe::lws_per_vhost_data *vhd =
    (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));

  AudioPipe ** ppAp = (AudioPipe **) user;

  switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
      vhd = (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(struct AudioPipe::lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
      {
        AudioPipe* ap = findPendingConnect(wsi);
        if (ap && ap->hasBasicAuth()) {
          unsigned char **p = (unsigned char **)in, *end = (*p) + len;
          char b[128];
          std::string username, password;

          ap->getBasicAuth(username, password);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER 用户名: %s, 密码: xxxxxx\n", username.c_str());
          if (dch_lws_http_basic_auth_gen(username.c_str(), password.c_str(), b, sizeof(b))) break;
          if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION, (unsigned char *)b, strlen(b), p, end)) return -1;
        }
      }
      break;

    case LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL:
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "AudioPipe::lws_service_thread LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL\n");
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      processPendingConnects(vhd);
      processPendingDisconnects(vhd);
      processPendingWrites();
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        int rc = lws_http_client_http_response(wsi);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR: %s, 响应状态 %d\n", in ? (char *)in : "(null)", rc);
        if (ap) {
          ap->m_state.store(LWS_CLIENT_FAILED, std::memory_order_release);
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECT_FAIL, (char *) in, NULL, len);
          delete ap;
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR 无法找到 wsi %p..\n", wsi);
        }
      }
      break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        if (ap) {
          *ppAp = ap;
          ap->m_vhd = vhd;
          ap->m_state.store(LWS_CLIENT_CONNECTED, std::memory_order_release);
          if (ap->m_delete_on_close.load(std::memory_order_acquire)) {
            addPendingDisconnect(ap);
          }
          else {
            ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECT_SUCCESS, NULL, NULL, len);
          }
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_ESTABLISHED 无法找到 wsi %p..\n", wsi);
        }
      }
      break;
    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CLOSED 无法找到 wsi %p..\n", wsi);
          return 0;
        }
        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          // 由我方关闭
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL, NULL, len);
        }
        else if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTED) {
          // 由远端关闭
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,"%s 套接字由远端关闭\n", ap->m_uuid.c_str());
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECTION_DROPPED, NULL, NULL, len);
        }
        ap->m_state.store(LWS_CLIENT_DISCONNECTED, std::memory_order_release);

        //注意：收到上述任何事件后，任何持有此对象
        //指针或引用的地方都必须将其视为不再有效

        *ppAp = NULL;
        removeFromPendingLists(ap);
        delete ap;
      }
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      {

        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 无法找到 wsi %p..\n", wsi);
          return 0;
        }

        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,"AudioPipe::lws_service_thread 竞态条件：关闭连接时收到传入消息。\n");
          return 0;
        }

        if (lws_frame_is_binary(wsi)) {
          if (len > 0 && ap->is_bidirectional_audio_stream()) {
            ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::BINARY, NULL, (char *) in, len);
          } else if (len > 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 收到意外的二进制帧，丢弃。\n");
          }
          else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 收到零长度二进制帧，丢弃。\n");
          }
        }
        else {
          if (lws_is_first_fragment(wsi)) {
            // 为所需的整块内存分配缓冲区
            assert(nullptr == ap->m_recv_buf);
            ap->m_recv_buf_len = len + lws_remaining_packet_payload(wsi);
            if (ap->m_recv_buf_len > MAX_RECV_BUF_SIZE) {
              ap->m_recv_buf_len = 0;
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 文本消息超过最大缓冲区，丢弃。\n");
              break;
            }
            ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);
            if (!ap->m_recv_buf) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe: malloc 失败，跳过消息\n");
              break;
            }
            ap->m_recv_buf_ptr = ap->m_recv_buf;
          }
          else if (!ap->m_recv_buf) {
            break;
          }

          size_t write_offset = ap->m_recv_buf_ptr - ap->m_recv_buf;
          size_t remaining_space = ap->m_recv_buf_len - write_offset;
          if (remaining_space < len) {
            size_t newlen = ap->m_recv_buf_len + RECV_BUF_REALLOC_SIZE;
            if (newlen > MAX_RECV_BUF_SIZE) {
              free(ap->m_recv_buf);
              ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
              ap->m_recv_buf_len = 0;
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 超过最大缓冲区，截断消息。\n");
            }
            else {
              uint8_t* newbuf = (uint8_t*) realloc(ap->m_recv_buf, newlen);
              if (newbuf) {
                ap->m_recv_buf = newbuf;
                ap->m_recv_buf_len = newlen;
                ap->m_recv_buf_ptr = newbuf + write_offset;
              }
              else {
                free(ap->m_recv_buf);
                ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
                ap->m_recv_buf_len = 0;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 扩容失败，丢弃消息。\n");
              }
            }
          }

          if (nullptr != ap->m_recv_buf) {
            if (len > 0) {
              memcpy(ap->m_recv_buf_ptr, in, len);
              ap->m_recv_buf_ptr += len;
            }
            if (lws_is_final_fragment(wsi)) {
              if (nullptr != ap->m_recv_buf) {
                std::string msg((char *)ap->m_recv_buf, ap->m_recv_buf_ptr - ap->m_recv_buf);
                ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::MESSAGE, msg.c_str(), NULL, len);
                if (nullptr != ap->m_recv_buf) free(ap->m_recv_buf);
              }
              ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
              ap->m_recv_buf_len = 0;
            }
          }
        }
      }
      break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE 无法找到 wsi %p..\n", wsi);
          return 0;
        }
        ap->m_write_pending.store(false, std::memory_order_release);

        // 检查优雅关闭 - 发送剩余音频后关闭连接
        if (ap->isGracefulShutdown()) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"%s 优雅关闭 - 发送剩余音频后关闭连接\n", ap->m_uuid.c_str());
          {
            std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
            if (ap->m_audio_buffer_write_offset > LWS_PRE) {
              size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
              int sent = lws_write(wsi, (unsigned char *) ap->m_audio_buffer + LWS_PRE, datalen, LWS_WRITE_BINARY);
              if (sent >= (int)datalen) {
                ap->m_audio_buffer_write_offset = LWS_PRE;
              }
              else if (sent > 0) {
                size_t remaining = datalen - (size_t)sent;
                memmove(ap->m_audio_buffer + LWS_PRE, ap->m_audio_buffer + LWS_PRE + sent, remaining);
                ap->m_audio_buffer_write_offset = LWS_PRE + remaining;
                lws_callback_on_writable(wsi);
                return 0;
              }
              else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                  "AudioPipe: 优雅关闭期间发送音频失败，关闭连接\n");
                return -1;
              }
            }
          }
          return -1;
        }

        // 检查待发送的文本帧
        {
          std::lock_guard<std::mutex> lk(ap->m_text_mutex);
          if (!ap->m_metadata_list.empty()) {
            const std::string& message = ap->m_metadata_list.front();
            size_t totalLen = message.length() + LWS_PRE;
            uint8_t* buf = (uint8_t*) malloc(totalLen);
            if (!buf) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "AudioPipe: failed to alloc write buffer\n");
              return -1;
            }
            memcpy(buf + LWS_PRE, message.c_str(), message.length());
            int n = message.length();
            int m = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            free(buf);

            if (m < n) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "AudioPipe: 文本帧未完整发送，关闭连接以避免重复发送\n");
              return -1;
            }

            // 移除已成功发送的消息
            ap->m_metadata_list.pop_front();
            // 如果还有更多消息则请求另一个可写事件
            lws_callback_on_writable(wsi);
            return 0;
          }
        }

        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
          return -1;
        }

        // 检查音频数据包
        {
          std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
          if (ap->m_audio_buffer_write_offset > LWS_PRE) {
            size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
            int sent = lws_write(wsi, (unsigned char *) ap->m_audio_buffer + LWS_PRE, datalen, LWS_WRITE_BINARY);
            if (sent < (int)datalen) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,"AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s 尝试发送 %lu 仅发送了 %d wsi %p..\n",
                ap->m_uuid.c_str(), datalen, sent, wsi);
              if (sent > 0) {
                size_t remaining = datalen - (size_t)sent;
                memmove(ap->m_audio_buffer + LWS_PRE, ap->m_audio_buffer + LWS_PRE + sent, remaining);
                ap->m_audio_buffer_write_offset = LWS_PRE + remaining;
                lws_callback_on_writable(wsi);
              }
              else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                  "AudioPipe: 音频帧发送失败，关闭连接\n");
                return -1;
              }
            }
            else {
              ap->m_audio_buffer_write_offset = LWS_PRE;
            }
          }
        }

        return 0;
      }
      break;

    default:
      break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}


// 静态成员
static const lws_retry_bo_t retry = {
    nullptr,   // 重试毫秒表
    0,         // 重试毫秒表计数
    0,         // 隐藏计数
    UINT16_MAX,         // 有效 ping 的秒数
    UINT16_MAX,        // 有效挂断的秒数
    0          // 抖动百分比
};

struct lws_context *AudioPipe::context = nullptr;
std::thread AudioPipe::serviceThread;
std::string AudioPipe::protocolName;
std::mutex AudioPipe::mutex_connects;
std::mutex AudioPipe::mutex_disconnects;
std::mutex AudioPipe::mutex_writes;
std::list<AudioPipe*> AudioPipe::pendingConnects;
std::unordered_map<struct lws*, AudioPipe*> AudioPipe::pendingConnectsByWsi;
std::list<AudioPipe*> AudioPipe::pendingDisconnects;
std::list<AudioPipe*> AudioPipe::pendingWrites;
AudioPipe::log_emit_function AudioPipe::logger;
std::mutex AudioPipe::mapMutex;
std::atomic<bool> AudioPipe::stopFlag{false};

void AudioPipe::processPendingConnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> connects;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
      if ((*it)->m_state.load(std::memory_order_acquire) == LWS_CLIENT_IDLE) {
        connects.push_back(*it);
        (*it)->m_state.store(LWS_CLIENT_CONNECTING, std::memory_order_release);
      }
    }
  }
  for (auto it = connects.begin(); it != connects.end(); ++it) {
    AudioPipe* ap = *it;
    if (!ap->connect_client(vhd)) {
      {
        std::lock_guard<std::mutex> guard(mutex_connects);
        pendingConnects.remove(ap);
        if (ap->m_wsi) pendingConnectsByWsi.erase(ap->m_wsi);
      }
      ap->m_state.store(LWS_CLIENT_FAILED, std::memory_order_release);
      ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECT_FAIL,
        "lws_client_connect_via_info returned null", NULL, 0);
      delete ap;
    }
    else {
      std::lock_guard<std::mutex> guard(mutex_connects);
      pendingConnects.remove(ap);
    }
  }
}

void AudioPipe::processPendingDisconnects(lws_per_vhost_data * /*vhd*/) {
  std::list<AudioPipe*> disconnects;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    for (auto it = pendingDisconnects.begin(); it != pendingDisconnects.end(); ++it) {
      LwsState_t state = (*it)->m_state.load(std::memory_order_acquire);
      if (state == LWS_CLIENT_DISCONNECTING ||
        (state == LWS_CLIENT_CONNECTING && (*it)->m_delete_on_close.load(std::memory_order_acquire))) {
        disconnects.push_back(*it);
      }
    }
    pendingDisconnects.clear();
  }
  for (auto it = disconnects.begin(); it != disconnects.end(); ++it) {
    AudioPipe* ap = *it;
    if (ap->m_wsi) {
      if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTING) {
        lws_set_timeout(ap->m_wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
      }
      else {
        lws_callback_on_writable(ap->m_wsi);
      }
    }
  }
}

void AudioPipe::processPendingWrites() {
  std::list<AudioPipe*> writes;
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    for (auto it = pendingWrites.begin(); it != pendingWrites.end(); ++it) {
       if ((*it)->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTED) writes.push_back(*it);
    }
    pendingWrites.clear();
  }
  for (auto it = writes.begin(); it != writes.end(); ++it) {
    AudioPipe* ap = *it;
    if (ap->m_wsi) lws_callback_on_writable(ap->m_wsi);
  }
}

AudioPipe* AudioPipe::findAndRemovePendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);
  std::list<AudioPipe* > toRemove;

  auto indexed = pendingConnectsByWsi.find(wsi);
  if (indexed != pendingConnectsByWsi.end()) {
    ap = indexed->second;
    pendingConnectsByWsi.erase(indexed);
  }
  else {
    AudioPipe* opaque = static_cast<AudioPipe*>(lws_get_opaque_user_data(wsi));
    if (opaque && opaque->m_wsi == wsi &&
      opaque->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTING) {
      ap = opaque;
    }
  }

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state.load(std::memory_order_acquire);

    if ((*it)->m_wsi == nullptr)
      toRemove.push_back(*it);

    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }

  for (auto it = toRemove.begin(); it != toRemove.end(); ++it)
    pendingConnects.remove(*it);

  if (ap) {
    pendingConnects.remove(ap);
  }

  return ap;
}

AudioPipe* AudioPipe::findPendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);

  auto indexed = pendingConnectsByWsi.find(wsi);
  if (indexed != pendingConnectsByWsi.end()) {
    ap = indexed->second;
  }
  else {
    AudioPipe* opaque = static_cast<AudioPipe*>(lws_get_opaque_user_data(wsi));
    if (opaque && opaque->m_wsi == wsi &&
      opaque->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTING) {
      ap = opaque;
    }
  }

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state.load(std::memory_order_acquire);
    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }
  return ap;
}

void AudioPipe::addPendingConnect(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnects.push_back(ap);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"%s 添加连接后有 %lu 个待处理连接\n",
      ap->m_uuid.c_str(), pendingConnects.size());
  }
  if (context) lws_cancel_service(context);
}
void AudioPipe::addPendingDisconnect(AudioPipe* ap) {
  if (ap->m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTING) {
    ap->m_state.store(LWS_CLIENT_DISCONNECTING, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    pendingDisconnects.push_back(ap);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"%s 添加断开连接后有 %lu 个待处理断开连接\n",
      ap->m_uuid.c_str(), pendingDisconnects.size());
  }
  if (ap->m_vhd) {
    lws_cancel_service(ap->m_vhd->context);
  } else if (context) {
    lws_cancel_service(context);
  }
}
void AudioPipe::addPendingWrite(AudioPipe* ap) {
  bool expected = false;
  if (!ap->m_write_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    pendingWrites.push_back(ap);
  }
  if (ap->m_vhd) {
    lws_cancel_service(ap->m_vhd->context);
  } else if (context) {
    lws_cancel_service(context);
  }
}

void AudioPipe::removeFromPendingLists(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnects.remove(ap);
    if (ap->m_wsi) pendingConnectsByWsi.erase(ap->m_wsi);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    pendingDisconnects.remove(ap);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    pendingWrites.remove(ap);
  }
  ap->m_write_pending.store(false, std::memory_order_release);
}

bool AudioPipe::lws_service_thread() {
  struct lws_context_creation_info info;

  const struct lws_protocols protocols[] = {
    {
      protocolName.c_str(),
      AudioPipe::lws_callback,
      sizeof(void *),
      1024,
      0,
      nullptr,
      0
    },
    { NULL, NULL, 0, 0, 0, nullptr, 0 }
  };

  memset(&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

  info.ka_time = nTcpKeepaliveSecs;                    // tcp keep-alive 计时器
  info.ka_probes = 4;                   // 关闭连接前尝试 keep-alive 的次数
  info.ka_interval = 5;                 // keep-alive 之间的时间间隔
  info.timeout_secs = 10;                // 文档说明这是"涉及网络往返的各种过程"的超时时间
  info.keepalive_timeout = 5;           // 允许远程客户端持有空闲 HTTP/1.1 连接的秒数
  info.timeout_secs_ah_idle = 10;       // 允许客户端持有但未使用 ah 的秒数
  info.retry_and_idle_policy = &retry;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,"AudioPipe::lws_service_thread 创建上下文\n");

  context = lws_create_context(&info);
  if (!context) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"AudioPipe::lws_service_thread 创建上下文失败\n");
    return false;
  }

  int n;
  do {
    n = lws_service(context, 0);
  } while (n >= 0 && !stopFlag.load(std::memory_order_acquire));

  lwsl_notice("AudioPipe::lws_service_thread 结束\n");
  lws_context_destroy(context);
  context = nullptr;

  return true;
}

void AudioPipe::initialize(const char* protocol, int loglevel, log_emit_function logger) {
  protocolName = protocol;
  lws_set_log_level(loglevel, logger);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,"AudioPipe::initialize 启动中\n");
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag.store(false, std::memory_order_release);
  serviceThread = std::thread(&AudioPipe::lws_service_thread);
}

bool AudioPipe::deinitialize() {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,"AudioPipe::deinitialize\n");
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag.store(true, std::memory_order_release);
  if (context) lws_cancel_service(context);
  if (serviceThread.joinable()) {
    serviceThread.join();
  }
  return true;
}

// 实例成员
AudioPipe::AudioPipe(const char* uuid, const char* host, unsigned int port, const char* path,
  int sslFlags, size_t bufLen, size_t minFreespace, const char* username, const char* password, char* bugname,
  int bidirectional_audio_stream, notifyHandler_t callback) :
  m_state(LWS_CLIENT_IDLE), m_uuid(uuid), m_host(host), m_bugname(bugname), m_port(port), m_path(path),
  m_sslFlags(sslFlags), m_wsi(nullptr), m_audio_buffer_max_len(bufLen),
  m_audio_buffer_write_offset(LWS_PRE), m_audio_buffer_min_freespace(minFreespace),
  m_recv_buf(nullptr), m_recv_buf_ptr(nullptr),
  m_recv_buf_len(0), m_vhd(nullptr), m_callback(callback), m_gracefulShutdown(false),
  m_delete_on_close(false), m_write_pending(false) {

  if (username && password) {
    m_username.assign(username);
    m_password.assign(password);
  }
  m_bidirectional_audio_stream = bidirectional_audio_stream;
  m_audio_buffer = new uint8_t[m_audio_buffer_max_len];
}
AudioPipe::~AudioPipe() {
  if (m_audio_buffer) delete [] m_audio_buffer;
  if (m_recv_buf) free(m_recv_buf);
}

void AudioPipe::connect(void) {
  addPendingConnect(this);
}

void AudioPipe::closeAndDestroy(void) {
  m_delete_on_close.store(true, std::memory_order_release);

  LwsState_t state = m_state.load(std::memory_order_acquire);
  if (state == LWS_CLIENT_CONNECTED) {
    addPendingDisconnect(this);
    return;
  }

  if (state == LWS_CLIENT_DISCONNECTING || state == LWS_CLIENT_CONNECTING) {
    if (state == LWS_CLIENT_CONNECTING) addPendingDisconnect(this);
    else if (m_vhd) lws_cancel_service(m_vhd->context);
    else if (context) lws_cancel_service(context);
    return;
  }

  removeFromPendingLists(this);
  delete this;
}

bool AudioPipe::connect_client(struct lws_per_vhost_data *vhd) {
  assert(m_audio_buffer != nullptr);
  assert(m_vhd == nullptr);

  struct lws_client_connect_info i;

  memset(&i, 0, sizeof(i));
  i.context = vhd->context;
  i.port = m_port;
  i.address = m_host.c_str();
  i.path = m_path.c_str();
  i.host = i.address;
  i.origin = i.address;
  i.ssl_connection = m_sslFlags;
  i.protocol = protocolName.c_str();
  i.pwsi = &(m_wsi);
  i.opaque_user_data = this;

  m_state.store(LWS_CLIENT_CONNECTING, std::memory_order_release);
  m_vhd = vhd;

  m_wsi = lws_client_connect_via_info(&i);
  if (m_wsi) {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnectsByWsi[m_wsi] = this;
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"%s 尝试连接，wsi 为 %p\n", m_uuid.c_str(), m_wsi);

  return nullptr != m_wsi;
}

void AudioPipe::bufferForSending(const char* text) {
  if (m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTED) return;
  if (!m_vhd) return;
  {
    std::lock_guard<std::mutex> lk(m_text_mutex);
    if (m_metadata_list.size() >= 100) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s 元数据队列已满，丢弃最旧消息\n", m_uuid.c_str());
      m_metadata_list.pop_front();
    }
    m_metadata_list.emplace_back(text);
  }
  addPendingWrite(this);
}

void AudioPipe::unlockAudioBuffer() {
  if (m_audio_buffer_write_offset > LWS_PRE) addPendingWrite(this);
  m_audio_mutex.unlock();
}

void AudioPipe::close() {
  if (m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTED) return;
  addPendingDisconnect(this);
}

void AudioPipe::do_graceful_shutdown() {
  if (m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTED) return;
  m_state.store(LWS_CLIENT_DISCONNECTING, std::memory_order_release);
  m_gracefulShutdown.store(true, std::memory_order_release);
  addPendingWrite(this);
}
