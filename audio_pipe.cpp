/**
 * @file audio_pipe.cpp
 * @brief libwebsockets 异步客户端封装与音频传输管道实现
 *
 * 核心机制说明:
 * 1. 跨线程通信机制:
 *    - FreeSWITCH 工作线程将请求 (连接/断开/写入) 追加至 pending 列表, 并调用
 *      lws_cancel_service(context) 打断 libwebsockets 的 poll 阻塞;
 *    - LWS 服务线程在 LWS_CALLBACK_EVENT_WAIT_CANCELLED 回调中批量消费 pending 队列.
 * 2. 内存与对象生命周期 (强引用计数保障):
 *    - FreeSWITCH 会话持有所有权引用 (由 releaseOwner 释放);
 *    - libwebsockets 底层句柄在活动期间持有 wsi 引用 (由 releaseWsiRef 释放);
 *    - 每一个在队列中待处理的异步操作均通过 addRef/release 配对保护;
 *    - 当且仅当 m_refCount 减为 0 时, 触发 safe_destroy() 真正 delete 对象.
 * 3. 帧首部预留 (LWS_PRE):
 *    - libwebsockets 发送二进制/文本帧要求缓冲区数据指针之前必须预留 LWS_PRE (通常为 16 字节)
 *      用于底层拼接 WebSocket 帧头, 避免二次内存拷贝.
 */

#include "audio_pipe.hpp"
#include <switch.h>

#include <cassert>
#include <iostream>
#include <new>
#include <memory>
#include <exception>
#include <vector>
#include <cerrno>
#include <climits>
#include <cstdlib>

/* 下行文本消息缓冲区上限 (650 KiB), 超过此长度判定为异常载荷并丢弃 */
#define MAX_RECV_BUF_SIZE (65 * 1024 * 10)
/* 下行文本碎片重组时每次动态扩容增量 (8 KiB) */
#define RECV_BUF_REALLOC_SIZE (8 * 1024)

/* 待发送元数据文本队列最大深度, 超过后丢弃最旧消息以保护内存 */
static const size_t MAX_METADATA_QUEUE_DEPTH = 100;
/* 上行音频环形缓冲区硬上限 (2 MiB), 防止异常配置耗尽内存 */
static const size_t MAX_AUDIO_PIPE_BUFFER_LIMIT = 2U * 1024U * 1024U;

using namespace drachtio;

namespace {
  /**
   * @brief 解析带有边界保护的环境变量整型值
   */
  static int bounded_env_int(const char *value, int fallback, int minimum, int maximum) {
    if (!value || !*value) return fallback;
    errno = 0;
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != 0) return fallback;
    if (parsed < minimum) return minimum;
    if (parsed > maximum) return maximum;
    return static_cast<int>(parsed);
  }

  static const char* basicAuthUser = std::getenv("MOD_AUDIO_FORK_HTTP_AUTH_USER");
  static const char* basicAuthPassword = std::getenv("MOD_AUDIO_FORK_HTTP_AUTH_PASSWORD");

  static const char *requestedTcpKeepaliveSecs = std::getenv("MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS");
  static int nTcpKeepaliveSecs = bounded_env_int(requestedTcpKeepaliveSecs, 55, 0, 86400);

  /**
   * @brief 从 std::list 中移除所有匹配 target 的指针并返回移除数量
   */
  static size_t remove_from_pipe_list(std::list<AudioPipe*>& list, AudioPipe* target) {
    size_t removed = 0;
    for (auto it = list.begin(); it != list.end();) {
      if (*it == target) {
        it = list.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }
}

/**
 * @brief 生成 HTTP Basic 认证头部字符串
 * @note 更新到内建此辅助函数的 lws 版本后可安全替换
 */
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
    /* --------------------------------------------------------------------- */
    /* 协议初始化: 为当前 vhost 分配上下文结构体                             */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_PROTOCOL_INIT:
      vhd = (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_zalloc(
        lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(struct AudioPipe::lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    /* --------------------------------------------------------------------- */
    /* 握手头追加: 若配置了 Basic Auth, 注入 Authorization HTTP 头部         */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
      {
        AudioPipe* ap = findPendingConnect(wsi);
        if (ap && ap->hasBasicAuth()) {
          unsigned char **p = (unsigned char **)in, *end = (*p) + len;
          char b[128];
          std::string username, password;

          ap->getBasicAuth(username, password);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER 用户名: %s, 密码: xxxxxx\n",
            username.c_str());
          if (dch_lws_http_basic_auth_gen(username.c_str(), password.c_str(), b, sizeof(b))) {
            ap->release();
            break;
          }
          if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION, (unsigned char *)b, strlen(b), p, end)) {
            ap->release();
            return -1;
          }
        }
        if (ap) ap->release();
      }
      break;

    case LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL:
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
        "AudioPipe::lws_service_thread LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL\n");
      break;

    /* --------------------------------------------------------------------- */
    /* 跨线程唤醒: FreeSWITCH 线程调用 lws_cancel_service 后在此触发处理队列 */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      processPendingConnects(vhd);
      processPendingDisconnects(vhd);
      processPendingWrites();
      break;

    /* --------------------------------------------------------------------- */
    /* 建连错误: 握手或网络失败, 撤销所有待处理状态并分发 CONNECT_FAIL 事件   */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        int rc = lws_http_client_http_response(wsi);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
          "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR: %s, 响应状态 %d\n",
          in ? (char *)in : "(null)", rc);
        if (ap) {
          ap->m_state.store(LWS_CLIENT_FAILED, std::memory_order_release);
          /* 连接失败也必须撤销尚未处理的 disconnect/write 引用, 避免保留失效对象 */
          removeFromPendingLists(ap);
          /* CLIENT_CONNECTION_ERROR 之后 libwebsockets 不再拥有可用 wsi; 清空避免后续重入 */
          ap->m_wsi = nullptr;
          ap->m_vhd = nullptr;
          if (ap->m_callback) {
            ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
              AudioPipe::CONNECT_FAIL, (char *) in, NULL, len);
          }
          /* connect_client 为 wsi 保留的引用在错误回调结束时释放 */
          ap->release(); /* findAndRemove 产生的临时持有引用 */
          ap->releaseWsiRef();
        } else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR 无法找到 wsi %p..\n", wsi);
        }
      }
      break;

    /* --------------------------------------------------------------------- */
    /* 建连成功: 握手完毕, 绑定会话指针并分发 CONNECT_SUCCESS 事件          */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        if (ap) {
          *ppAp = ap;
          ap->m_vhd = vhd;
          ap->m_state.store(LWS_CLIENT_CONNECTED, std::memory_order_release);
          if (ap->m_delete_on_close.load(std::memory_order_acquire)) {
            addPendingDisconnect(ap);
          } else {
            if (ap->m_callback) {
              ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
                AudioPipe::CONNECT_SUCCESS, NULL, NULL, len);
            }
          }
          ap->release();
        } else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_ESTABLISHED 无法找到 wsi %p..\n", wsi);
        }
      }
      break;

    /* --------------------------------------------------------------------- */
    /* 连接关闭: 判断是由我方主动关闭还是远端挂断并向业务层上报对应事件     */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CLOSED 无法找到 wsi %p..\n", wsi);
          return 0;
        }
        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          /* 我方主动请求断开, 上报优雅关闭事件 */
          if (ap->m_callback) {
            ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
              AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL, NULL, len);
          }
        } else if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTED) {
          /* 远端服务器断开, 上报连接丢失事件 */
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s 套接字由远端关闭\n", ap->m_uuid.c_str());
          if (ap->m_callback) {
            ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
              AudioPipe::CONNECTION_DROPPED, NULL, NULL, len);
          }
        }
        ap->m_state.store(LWS_CLIENT_DISCONNECTED, std::memory_order_release);

        *ppAp = NULL;
        removeFromPendingLists(ap);
        /* CLIENT_CLOSED 之后 wsi 已经失效; 清空句柄并归还 LWS 持有的引用 */
        ap->m_wsi = nullptr;
        ap->m_vhd = nullptr;
        ap->releaseWsiRef();
      }
      break;

    /* --------------------------------------------------------------------- */
    /* 下行数据接收: 处理二进制 PCM 音频或文本 (JSON) 信令分片重组           */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_RECEIVE:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 无法找到 wsi %p..\n", wsi);
          return 0;
        }

        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "AudioPipe::lws_service_thread 竞态条件: 关闭连接时收到传入消息.\n");
          return 0;
        }

        /* 1. 二进制音频帧处理 (TTS/全双工下行裸 PCM) */
        if (lws_frame_is_binary(wsi)) {
          if (ap->is_bidirectional_audio_stream()) {
            if (len == 0) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                "AudioPipe::lws_service_thread (%s) 收到零长二进制帧 (EOF 定稿标记)\n", ap->m_uuid.c_str());
            }
            if (ap->m_callback) {
              ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
                AudioPipe::BINARY, NULL, (char *) in, len);
            }
          } else if (len > 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
              "AudioPipe::lws_service_thread (%s) 收到意外的二进制帧(双向流未启用), 丢弃.\n", ap->m_uuid.c_str());
          } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
              "AudioPipe::lws_service_thread (%s) 收到零长度二进制帧, 丢弃.\n", ap->m_uuid.c_str());
          }
        }
        /* 2. 文本信令帧处理 (JSON 分片动态拼接) */
        else {
          if (lws_is_first_fragment(wsi)) {
            assert(nullptr == ap->m_recv_buf);
            size_t remaining_payload = lws_remaining_packet_payload(wsi);
            if (remaining_payload > MAX_RECV_BUF_SIZE || len > MAX_RECV_BUF_SIZE - remaining_payload) {
              ap->m_recv_buf_len = 0;
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "AudioPipe::lws_service_thread 文本消息长度计算溢出, 丢弃.\n");
              break;
            }
            ap->m_recv_buf_len = len + remaining_payload;
            if (ap->m_recv_buf_len > MAX_RECV_BUF_SIZE) {
              ap->m_recv_buf_len = 0;
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 文本消息超过最大缓冲区, 丢弃.\n");
              break;
            }
            ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);
            if (!ap->m_recv_buf) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe: malloc 失败, 跳过消息\n");
              break;
            }
            ap->m_recv_buf_ptr = ap->m_recv_buf;
          } else if (!ap->m_recv_buf) {
            break;
          }

          size_t write_offset = ap->m_recv_buf_ptr - ap->m_recv_buf;
          if (write_offset > ap->m_recv_buf_len || write_offset > MAX_RECV_BUF_SIZE || len > MAX_RECV_BUF_SIZE - write_offset) {
            free(ap->m_recv_buf);
            ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
            ap->m_recv_buf_len = 0;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
              "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 超过最大缓冲区, 丢弃消息.\n");
            break;
          }
          size_t remaining_space = ap->m_recv_buf_len - write_offset;
          if (remaining_space < len) {
            size_t required = write_offset + len;
            size_t grown = ap->m_recv_buf_len;
            if (grown < required) {
              size_t incremented = grown > MAX_RECV_BUF_SIZE - RECV_BUF_REALLOC_SIZE
                ? MAX_RECV_BUF_SIZE : grown + RECV_BUF_REALLOC_SIZE;
              grown = incremented > required ? incremented : required;
            }
            if (grown > MAX_RECV_BUF_SIZE) {
              free(ap->m_recv_buf);
              ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
              ap->m_recv_buf_len = 0;
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 超过最大缓冲区, 丢弃消息.\n");
            } else {
              uint8_t* newbuf = (uint8_t*) realloc(ap->m_recv_buf, grown);
              if (newbuf) {
                ap->m_recv_buf = newbuf;
                ap->m_recv_buf_len = grown;
                ap->m_recv_buf_ptr = newbuf + write_offset;
              } else {
                free(ap->m_recv_buf);
                ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
                ap->m_recv_buf_len = 0;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                  "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE 扩容失败, 丢弃消息.\n");
              }
            }
          }

          if (nullptr != ap->m_recv_buf) {
            if (len > 0) {
              memcpy(ap->m_recv_buf_ptr, in, len);
              ap->m_recv_buf_ptr += len;
            }
            /* 当收到消息的最终分片 (final fragment) 时, 分发完整文本事件 */
            if (lws_is_final_fragment(wsi)) {
              if (nullptr != ap->m_recv_buf) {
                try {
                  std::string msg((char *)ap->m_recv_buf, ap->m_recv_buf_ptr - ap->m_recv_buf);
                  if (ap->m_callback) {
                    ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
                      AudioPipe::MESSAGE, msg.c_str(), NULL, len);
                  }
                } catch (const std::exception& ex) {
                  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                    "AudioPipe: 文本消息分配失败: %s\n", ex.what());
                } catch (...) {
                  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                    "AudioPipe: 文本消息分配失败\n");
                }
                free(ap->m_recv_buf);
              }
              ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
              ap->m_recv_buf_len = 0;
            }
          }
        }
      }
      break;

    /* --------------------------------------------------------------------- */
    /* 上行数据可写: 发送排队的文本元数据或上行音频 PCM 数据包              */
    /* --------------------------------------------------------------------- */
    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE 无法找到 wsi %p..\n", wsi);
          return 0;
        }
        ap->m_write_pending.store(false, std::memory_order_release);

        /* 1. 检查优雅关闭: 发送完剩余音频后立即主动断开连接 */
        if (ap->isGracefulShutdown()) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "%s 优雅关闭 - 发送剩余音频后关闭连接\n", ap->m_uuid.c_str());
          {
            std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
            if (ap->m_audio_buffer_write_offset > LWS_PRE) {
              size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
              int sent = lws_write(wsi, (unsigned char *) ap->m_audio_buffer + LWS_PRE, datalen, LWS_WRITE_BINARY);
              if (sent >= (int)datalen) {
                ap->m_audio_buffer_write_offset = LWS_PRE;
              } else if (sent > 0) {
                size_t remaining = datalen - (size_t)sent;
                memmove(ap->m_audio_buffer + LWS_PRE, ap->m_audio_buffer + LWS_PRE + sent, remaining);
                ap->m_audio_buffer_write_offset = LWS_PRE + remaining;
                lws_callback_on_writable(wsi);
                return 0;
              } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                  "AudioPipe: 优雅关闭期间发送音频失败, 关闭连接\n");
                return -1;
              }
            }
          }
          return -1;
        }

        /* 2. 检查待发送的控制文本/元数据帧 (优先于音频发送) */
        {
          std::lock_guard<std::mutex> lk(ap->m_text_mutex);
          if (!ap->m_metadata_list.empty()) {
            const std::string& message = ap->m_metadata_list.front();
            size_t totalLen = message.length() + LWS_PRE;
            uint8_t* buf = (uint8_t*) malloc(totalLen);
            if (!buf) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe: failed to alloc write buffer\n");
              return -1;
            }
            memcpy(buf + LWS_PRE, message.c_str(), message.length());
            int n = message.length();
            int m = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            free(buf);

            if (m < n) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "AudioPipe: 文本帧未完整发送, 关闭连接以避免重复发送\n");
              return -1;
            }

            /* 移除已成功发送的消息 */
            ap->m_metadata_list.pop_front();
            /* 若还有待发送文本, 继续请求可写通知 */
            lws_callback_on_writable(wsi);
            return 0;
          }
        }

        if (ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_DISCONNECTING) {
          lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
          return -1;
        }

        /* 3. 发送上行音频 PCM 数据包 */
        {
          std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
          if (ap->m_audio_buffer_write_offset > LWS_PRE) {
            size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
            int sent = lws_write(wsi, (unsigned char *) ap->m_audio_buffer + LWS_PRE, datalen, LWS_WRITE_BINARY);
            if (sent < (int)datalen) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s 尝试发送 %lu 仅发送了 %d wsi %p..\n",
                ap->m_uuid.c_str(), datalen, sent, wsi);
              if (sent > 0) {
                size_t remaining = datalen - (size_t)sent;
                memmove(ap->m_audio_buffer + LWS_PRE, ap->m_audio_buffer + LWS_PRE + sent, remaining);
                ap->m_audio_buffer_write_offset = LWS_PRE + remaining;
                lws_callback_on_writable(wsi);
              } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "AudioPipe: 音频帧发送失败, 关闭连接\n");
                return -1;
              }
            } else {
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

/* 静态成员重试策略配置 */
static const lws_retry_bo_t retry = {
  nullptr,      // 重试毫秒表
  0,            // 重试毫秒表计数
  0,            // 隐藏计数
  UINT16_MAX,   // 有效 ping 的秒数
  UINT16_MAX,   // 有效挂断的秒数
  0             // 抖动百分比
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
std::mutex AudioPipe::instancesMutex;
std::unordered_set<AudioPipe*> AudioPipe::instances;
std::atomic<bool> AudioPipe::stopFlag{false};
std::atomic<bool> AudioPipe::shuttingDown{false};

void AudioPipe::processPendingConnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> connects;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
      if ((*it)->m_state.load(std::memory_order_acquire) == LWS_CLIENT_IDLE) {
        (*it)->addRef();
        connects.push_back(*it);
        (*it)->m_state.store(LWS_CLIENT_CONNECTING, std::memory_order_release);
      }
    }
  }
  for (auto it = connects.begin(); it != connects.end(); ++it) {
    AudioPipe* ap = *it;
    if (!ap->connect_client(vhd)) {
      const bool already_notified = ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_FAILED;
      removeFromPendingLists(ap);
      ap->m_state.store(LWS_CLIENT_FAILED, std::memory_order_release);
      if (!already_notified && ap->m_callback) {
        ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation, AudioPipe::CONNECT_FAIL,
          "lws_client_connect_via_info returned null", NULL, 0);
      }
      /* pendingConnects 的引用已由 removeFromPendingLists 释放 */
      ap->release(); /* 临时处理引用 */
    } else {
      size_t removed = 0;
      {
        std::lock_guard<std::mutex> guard(mutex_connects);
        removed = remove_from_pipe_list(pendingConnects, ap);
      }
      while (removed--) ap->release(); /* pendingConnects 引用 */
      ap->release(); /* 临时处理引用 */
    }
  }
}

void AudioPipe::processPendingDisconnects(lws_per_vhost_data * /*vhd*/) {
  std::list<AudioPipe*> disconnects;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    disconnects.splice(disconnects.end(), pendingDisconnects);
  }
  for (AudioPipe *ap : disconnects) {
    /* 当前循环接管了 pendingDisconnects 引用; 允许后续 stop 再次排队 */
    ap->m_disconnect_pending.store(false, std::memory_order_release);
    LwsState_t state = ap->m_state.load(std::memory_order_acquire);
    if (state == LWS_CLIENT_CONNECTING && !ap->m_wsi) {
      /* stop 与 connect_client 建立 wsi 的窗口: 撤销待连接请求, 避免悬挂在 CONNECTING */
      removeFromPendingLists(ap);
      ap->m_state.store(LWS_CLIENT_FAILED, std::memory_order_release);
      if (ap->m_callback) {
        ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
          AudioPipe::CONNECT_FAIL, "connection cancelled before websocket creation", NULL, 0);
      }
      ap->releaseOwner();
    } else if (ap->m_wsi) {
      if (state == LWS_CLIENT_CONNECTING) {
        lws_set_timeout(ap->m_wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
      } else {
        lws_callback_on_writable(ap->m_wsi);
      }
    } else {
      /* 没有 wsi 的已失败/空闲对象也要发收尾事件, 清理 FreeSWITCH 侧句柄 */
      removeFromPendingLists(ap);
      ap->m_state.store(LWS_CLIENT_DISCONNECTED, std::memory_order_release);
      if (ap->m_callback) {
        ap->m_callback(ap, ap->m_uuid.c_str(), ap->m_bugname.c_str(), ap->m_generation,
          AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL, NULL, 0);
      }
      ap->releaseOwner();
    }
    ap->release(); /* pendingDisconnects 引用 */
  }
}

void AudioPipe::processPendingWrites() {
  std::list<AudioPipe*> writes;
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    /* 接管全部排队的待写引用 */
    writes.splice(writes.end(), pendingWrites);
  }
  for (AudioPipe *ap : writes) {
    ap->m_write_pending.store(false, std::memory_order_release);
    if (ap->m_wsi && ap->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTED) {
      lws_callback_on_writable(ap->m_wsi);
    }
    ap->release(); /* pendingWrites 引用 */
  }
}

AudioPipe* AudioPipe::findAndRemovePendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);
  auto indexed = pendingConnectsByWsi.find(wsi);
  if (indexed != pendingConnectsByWsi.end()) {
    ap = indexed->second;
    pendingConnectsByWsi.erase(indexed);
  } else {
    AudioPipe* opaque = static_cast<AudioPipe*>(lws_get_opaque_user_data(wsi));
    if (opaque && opaque->m_wsi == wsi &&
      opaque->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTING) {
      ap = opaque;
    }
  }

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state.load(std::memory_order_acquire);
    if ((state == LWS_CLIENT_CONNECTING) && (*it)->m_wsi == wsi) ap = *it;
  }
  if (ap) {
    size_t removed = remove_from_pipe_list(pendingConnects, ap);
    /* 返回给回调的临时引用, 调用方必须 release */
    ap->addRef();
    while (removed--) ap->release(); /* pendingConnects 引用 */
  }

  return ap;
}

AudioPipe* AudioPipe::findPendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);

  auto indexed = pendingConnectsByWsi.find(wsi);
  if (indexed != pendingConnectsByWsi.end()) {
    ap = indexed->second;
  } else {
    AudioPipe* opaque = static_cast<AudioPipe*>(lws_get_opaque_user_data(wsi));
    if (opaque && opaque->m_wsi == wsi &&
      opaque->m_state.load(std::memory_order_acquire) == LWS_CLIENT_CONNECTING) {
      ap = opaque;
    }
  }

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state.load(std::memory_order_acquire);
    if ((state == LWS_CLIENT_CONNECTING) && (*it)->m_wsi == wsi) ap = *it;
  }
  if (ap) ap->addRef();
  return ap;
}

bool AudioPipe::addPendingConnect(AudioPipe* ap) {
  if (!ap) return false;
  ap->addRef();
  try {
    {
      std::lock_guard<std::mutex> guard(mutex_connects);
      pendingConnects.push_back(ap);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"%s 添加连接后有 %lu 个待处理连接\n",
        ap->m_uuid.c_str(), pendingConnects.size());
    }
    if (context) lws_cancel_service(context);
    return true;
  } catch (...) {
    ap->release();
    throw;
  }
}

void AudioPipe::addPendingDisconnect(AudioPipe* ap) {
  if (!ap) return;
  ap->addRef();
  try {
    {
      std::lock_guard<std::mutex> guard(mutex_disconnects);
      bool expected = false;
      if (!ap->m_disconnect_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ap->release();
        return;
      }
      if (ap->m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTING) {
        ap->m_state.store(LWS_CLIENT_DISCONNECTING, std::memory_order_release);
      }
      pendingDisconnects.push_back(ap);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "%s 添加断开连接后有 %lu 个待处理断开连接\n",
        ap->m_uuid.c_str(), pendingDisconnects.size());
    }
    if (context) {
      lws_cancel_service(context);
    }
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "%s 添加断开连接请求失败\n", ap->m_uuid.c_str());
    ap->m_disconnect_pending.store(false, std::memory_order_release);
    ap->release();
  }
}

void AudioPipe::addPendingWrite(AudioPipe* ap) {
  if (!ap) return;
  ap->addRef();
  try {
    {
      std::lock_guard<std::mutex> guard(mutex_writes);
      bool expected = false;
      if (!ap->m_write_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ap->release();
        return;
      }
      pendingWrites.push_back(ap);
    }
    if (context) {
      lws_cancel_service(context);
    }
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "%s 添加写请求失败\n", ap->m_uuid.c_str());
    ap->m_write_pending.store(false, std::memory_order_release);
    ap->release();
  }
}

void AudioPipe::removeFromPendingLists(AudioPipe* ap) {
  if (!ap) return;
  size_t removed = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    removed += remove_from_pipe_list(pendingConnects, ap);
    if (ap->m_wsi) pendingConnectsByWsi.erase(ap->m_wsi);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    removed += remove_from_pipe_list(pendingDisconnects, ap);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    removed += remove_from_pipe_list(pendingWrites, ap);
  }
  ap->m_write_pending.store(false, std::memory_order_release);
  ap->m_disconnect_pending.store(false, std::memory_order_release);
  while (removed--) ap->release();
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

  info.ka_time = nTcpKeepaliveSecs;     // TCP keep-alive 空闲探测计时器 (秒)
  info.ka_probes = 4;                    // 关闭连接前尝试 keep-alive 探测次数
  info.ka_interval = 5;                  // keep-alive 探测之间的时间间隔 (秒)
  info.timeout_secs = 10;                // 网络往返各种过程的全局超时 (秒)
  info.keepalive_timeout = 5;            // 空闲 HTTP/1.1 连接保持秒数
  info.timeout_secs_ah_idle = 10;        // 允许客户端持有但未使用的 ah 秒数
  info.retry_and_idle_policy = &retry;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "AudioPipe::lws_service_thread 创建上下文\n");

  context = lws_create_context(&info);
  if (!context) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe::lws_service_thread 创建上下文失败\n");
    return false;
  }

  int n = 0;
  try {
    do {
      n = lws_service(context, 0);
    } while (n >= 0 && !stopFlag.load(std::memory_order_acquire));
  } catch (const std::exception& ex) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::lws_service_thread 异常退出: %s\n", ex.what());
    stopFlag.store(true, std::memory_order_release);
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::lws_service_thread 异常退出\n");
    stopFlag.store(true, std::memory_order_release);
  }

  lwsl_notice("AudioPipe::lws_service_thread 结束\n");
  lws_context_destroy(context);
  context = nullptr;

  return true;
}

bool AudioPipe::initialize(const char* protocol, int loglevel, log_emit_function logger) {
  try {
    protocolName = protocol ? protocol : "";
    lws_set_log_level(loglevel, logger);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::initialize 启动中\n");
    std::lock_guard<std::mutex> lock(mapMutex);
    stopFlag.store(false, std::memory_order_release);
    shuttingDown.store(false, std::memory_order_release);
    serviceThread = std::thread(&AudioPipe::lws_service_thread);
    return true;
  } catch (const std::exception& ex) {
    stopFlag.store(true, std::memory_order_release);
    shuttingDown.store(true, std::memory_order_release);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::initialize 启动服务线程失败: %s\n", ex.what());
  } catch (...) {
    stopFlag.store(true, std::memory_order_release);
    shuttingDown.store(true, std::memory_order_release);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::initialize 启动服务线程失败\n");
  }
  return false;
}

bool AudioPipe::deinitialize() {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::deinitialize\n");
  try {
    std::lock_guard<std::mutex> lock(mapMutex);
    shuttingDown.store(true, std::memory_order_release);
    std::vector<AudioPipe*> all;
    {
      std::lock_guard<std::mutex> guard(instancesMutex);
      all.reserve(instances.size());
      for (AudioPipe *ap : instances) {
        ap->addRef();
        all.push_back(ap);
      }
    }

    for (AudioPipe *ap : all) {
      ap->m_delete_on_close.store(true, std::memory_order_release);
      addPendingDisconnect(ap);
    }
    stopFlag.store(true, std::memory_order_release);
    if (context) lws_cancel_service(context);
    if (serviceThread.joinable()) {
      serviceThread.join();
    }

    {
      std::lock_guard<std::mutex> guard(mutex_connects);
      for (AudioPipe *ap : pendingConnects) ap->release();
      pendingConnects.clear();
      pendingConnectsByWsi.clear();
    }
    {
      std::lock_guard<std::mutex> guard(mutex_disconnects);
      for (AudioPipe *ap : pendingDisconnects) ap->release();
      pendingDisconnects.clear();
    }
    {
      std::lock_guard<std::mutex> guard(mutex_writes);
      for (AudioPipe *ap : pendingWrites) {
        ap->m_write_pending.store(false, std::memory_order_release);
        ap->release();
      }
      pendingWrites.clear();
    }

    for (AudioPipe *ap : all) {
      ap->m_vhd = nullptr;
      ap->m_wsi = nullptr;
      ap->m_state.store(LWS_CLIENT_DISCONNECTED, std::memory_order_release);
      ap->m_disconnect_pending.store(false, std::memory_order_release);
      ap->releaseOwner();
      ap->releaseWsiRef();
      ap->release();
    }
    return true;
  } catch (const std::exception& ex) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::deinitialize 异常: %s\n", ex.what());
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::deinitialize 异常\n");
  }
  stopFlag.store(true, std::memory_order_release);
  shuttingDown.store(true, std::memory_order_release);
  if (context) lws_cancel_service(context);
  if (serviceThread.joinable()) {
    try {
      serviceThread.join();
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "AudioPipe::deinitialize 等待服务线程失败\n");
    }
  }
  return false;
}

AudioPipe::AudioPipe(const char* uuid, const char* host, unsigned int port, const char* path,
  int sslFlags, size_t bufLen, size_t minFreespace, const char* username, const char* password, char* bugname,
  uint64_t generation, int bidirectional_audio_stream, notifyHandler_t callback) :
  m_state(LWS_CLIENT_IDLE), m_uuid(uuid ? uuid : ""), m_host(host ? host : ""),
  m_bugname(bugname ? bugname : ""), m_generation(generation), m_port(port), m_path(path ? path : ""),
  m_sslFlags(sslFlags), m_wsi(nullptr), m_audio_buffer(nullptr), m_audio_buffer_max_len(bufLen),
  m_audio_buffer_write_offset(LWS_PRE), m_audio_buffer_min_freespace(minFreespace),
  m_recv_buf(nullptr), m_recv_buf_ptr(nullptr),
  m_recv_buf_len(0), m_vhd(nullptr), m_callback(callback), m_gracefulShutdown(false),
  m_delete_on_close(false), m_owner_released(false), m_valid(false), m_disconnect_pending(false),
  m_wsi_ref(false), m_refCount(1), m_write_pending(false) {

  if (username && password) {
    m_username.assign(username);
    m_password.assign(password);
  }
  m_bidirectional_audio_stream = bidirectional_audio_stream;
  {
    std::lock_guard<std::mutex> guard(instancesMutex);
    instances.insert(this);
  }
  if (m_audio_buffer_max_len == 0 || m_audio_buffer_max_len < LWS_PRE ||
      m_audio_buffer_max_len > MAX_AUDIO_PIPE_BUFFER_LIMIT ||
      m_audio_buffer_min_freespace > m_audio_buffer_max_len - LWS_PRE) {
    return;
  }
  m_audio_buffer = new (std::nothrow) uint8_t[m_audio_buffer_max_len];
  if (m_audio_buffer) m_valid.store(true, std::memory_order_release);
}

AudioPipe::~AudioPipe() {
  {
    std::lock_guard<std::mutex> guard(instancesMutex);
    instances.erase(this);
  }
  if (m_audio_buffer) delete [] m_audio_buffer;
  if (m_recv_buf) free(m_recv_buf);
}

void AudioPipe::addRef(void) {
  m_refCount.fetch_add(1, std::memory_order_relaxed);
}

void AudioPipe::release(void) {
  unsigned int refs = m_refCount.load(std::memory_order_acquire);
  while (refs != 0 && !m_refCount.compare_exchange_weak(refs, refs - 1,
      std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (refs == 1) {
    safe_destroy();
  }
}

void AudioPipe::safe_destroy(void) {
  /* 仅当所有引用归零时由 RAII 所有权负责 delete this */
  std::unique_ptr<AudioPipe> owner(this);
}

void AudioPipe::releaseOwner(void) {
  bool expected = false;
  if (m_owner_released.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    release();
  }
}

void AudioPipe::releaseWsiRef(void) {
  bool expected = true;
  if (m_wsi_ref.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    release();
  }
}

bool AudioPipe::connect(void) {
  if (!isValid() || shuttingDown.load(std::memory_order_acquire) ||
      m_delete_on_close.load(std::memory_order_acquire) ||
      m_state.load(std::memory_order_acquire) != LWS_CLIENT_IDLE) return false;
  try {
    return addPendingConnect(this);
  } catch (const std::exception& ex) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::connect 排队失败: %s\n", ex.what());
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::connect 排队失败\n");
  }
  return false;
}

void AudioPipe::closeAndDestroy(void) {
  m_delete_on_close.store(true, std::memory_order_release);

  LwsState_t state = m_state.load(std::memory_order_acquire);
  if (state == LWS_CLIENT_DISCONNECTING) {
    /* 已在断开中, 仅需唤醒 LWS 服务线程推动剩余事件 */
    if (context) lws_cancel_service(context);
  } else {
    /* 处于 CONNECTED、CONNECTING 或尚未创建 wsi, 均统一排入队列由 LWS 线程安全处理 */
    addPendingDisconnect(this);
  }
  releaseOwner();
}

bool AudioPipe::connect_client(struct lws_per_vhost_data *vhd) {
  if (!isValid() || !vhd || !vhd->context || m_delete_on_close.load(std::memory_order_acquire)) return false;
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

  /* libwebsockets 的 userdata 在连接关闭回调前一直持有这一引用 */
  addRef();
  m_wsi_ref.store(true, std::memory_order_release);
  m_wsi = lws_client_connect_via_info(&i);
  if (m_wsi) {
    try {
      std::lock_guard<std::mutex> guard(mutex_connects);
      pendingConnectsByWsi[m_wsi] = this;
    } catch (const std::exception& ex) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "AudioPipe: 保存连接索引失败: %s\n", ex.what());
      m_delete_on_close.store(true, std::memory_order_release);
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioPipe: 保存连接索引失败\n");
      m_delete_on_close.store(true, std::memory_order_release);
    }
  } else {
    releaseWsiRef();
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s 尝试连接, wsi 为 %p\n", m_uuid.c_str(), m_wsi);

  return nullptr != m_wsi;
}

void AudioPipe::bufferForSending(const char* text) {
  if (!text || m_state.load(std::memory_order_acquire) != LWS_CLIENT_CONNECTED) return;
  try {
    {
      std::lock_guard<std::mutex> lk(m_text_mutex);
      if (m_metadata_list.size() >= MAX_METADATA_QUEUE_DEPTH) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "%s 元数据队列已满, 丢弃最旧消息\n", m_uuid.c_str());
        m_metadata_list.pop_front();
      }
      m_metadata_list.emplace_back(text);
    }
    addPendingWrite(this);
  } catch (const std::exception& ex) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::bufferForSending 排队失败: %s\n", ex.what());
  } catch (...) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
      "AudioPipe::bufferForSending 排队失败\n");
  }
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
