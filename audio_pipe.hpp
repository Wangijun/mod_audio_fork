/**
 * @file audio_pipe.hpp
 * @brief libwebsockets 异步客户端封装与音频传输管道头文件
 *
 * 核心架构与职责:
 * 1. 管理单个通话通道到远端 WebSocket ASR/TTS 服务的全双工长连接;
 * 2. 线程解耦模型:
 *    - FreeSWITCH 业务线程: 产生音频帧 (fork_frame)、下发控制信令 (bufferForSending)、请求关闭 (closeAndDestroy);
 *    - libwebsockets 服务线程 (serviceThread): 执行事件循环 (lws_service), 驱动握手、读写回调与保活心跳;
 * 3. 强内存安全保障:
 *    - 基于原子引用计数 (m_refCount), 区分 FreeSWITCH 所有权 (m_owner_released) 与 LWS wsi 回调所有权 (m_wsi_ref);
 *    - 对象销毁永远延迟到所有跨线程操作归零后由 safe_destroy() 安全释放.
 */

#ifndef __AUDIO_PIPE_HPP__
#define __AUDIO_PIPE_HPP__

#include <string>
#include <list>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <cstdint>

#include <libwebsockets.h>

namespace drachtio {

  class AudioPipe {
  public:
    /**
     * @brief WebSocket 客户端连接状态机枚举
     */
    enum LwsState_t {
      LWS_CLIENT_IDLE,          /**< 初始空闲状态, 尚未发起建连 */
      LWS_CLIENT_CONNECTING,    /**< 正在进行 TCP 握手与 HTTP 协议升级 */
      LWS_CLIENT_CONNECTED,     /**< WebSocket 连接已建立, 可双向收发 */
      LWS_CLIENT_FAILED,        /**< 建连失败 (DNS解析/拒绝连接/鉴权未通过等) */
      LWS_CLIENT_DISCONNECTING, /**< 正在协商关闭中 */
      LWS_CLIENT_DISCONNECTED   /**< 连接已彻底断开 */
    };

    /**
     * @brief 异步上报至 FreeSWITCH 业务层的事件类型
     */
    enum NotifyEvent_t {
      CONNECT_SUCCESS,              /**< WebSocket 握手成功建立连接 */
      CONNECT_FAIL,                 /**< WebSocket 建连失败 */
      CONNECTION_DROPPED,           /**< 连接被远端异常切断 */
      CONNECTION_CLOSED_GRACEFULLY, /**< 经由正常信令协商优雅关闭 */
      MESSAGE,                      /**< 收到下行 UTF-8 文本消息 (通常为 JSON) */
      BINARY                        /**< 收到下行裸二进制音频数据帧 (S16LE PCM) */
    };

    typedef void (*log_emit_function)(int level, const char *line);

    /**
     * @brief 异步事件通知回调函数指针类型
     */
    typedef void (*notifyHandler_t)(AudioPipe *pipe, const char *sessionId, const char* bugname, uint64_t generation,
      NotifyEvent_t event, const char* message, const char* binary, size_t binary_len);

    /**
     * @brief 每个 libwebsockets vhost 绑定的用户数据
     */
    struct lws_per_vhost_data {
      struct lws_context *context;
      struct lws_vhost *vhost;
      const struct lws_protocols *protocol;
    };

    /**
     * @brief 初始化全局 libwebsockets 上下文与后台服务线程
     * @param protocolName WebSocket 子协议名称 (例如 audio.drachtio.org)
     * @param loglevel 日志过滤级别位掩码
     * @param logger 日志输出回调函数
     * @return true 启动成功; false 启动失败
     */
    static bool initialize(const char* protocolName, int loglevel, log_emit_function logger);

    /**
     * @brief 反初始化并安全停止全局服务线程
     */
    static bool deinitialize();

    /**
     * @brief 全局 libwebsockets 后台事件处理循环入口
     */
    static bool lws_service_thread();

    /**
     * @brief AudioPipe 构造函数
     * @param uuid FreeSWITCH 会话 UUID
     * @param host 目标主机域名或 IP
     * @param port 目标端口
     * @param path WebSocket 请求路径 (含查询参数)
     * @param sslFlags SSL/TLS 校验配置标志位 (例如是否允许自签名)
     * @param bufLen 上行 PCM 环形缓冲区总容量 (含 LWS_PRE 头保留)
     * @param minFreespace 写入上行音频所需的最小空闲门限
     * @param username HTTP Basic 认证用户名 (可选)
     * @param password HTTP Basic 认证密码 (可选)
     * @param bugname 关联的 FreeSWITCH media bug 名称
     * @param generation 通话代际编号
     * @param bidirectional_audio 是否启用全双工下行音频接收
     * @param callback 异步事件分发通知回调
     */
    AudioPipe(const char* uuid, const char* host, unsigned int port, const char* path, int sslFlags, 
      size_t bufLen, size_t minFreespace, const char* username, const char* password, char* bugname,
      uint64_t generation, int bidirectional_audio, notifyHandler_t callback);

    ~AudioPipe();  

    /** @brief 获取当前 WebSocket 客户端状态 */
    LwsState_t getLwsState(void) { return m_state.load(std::memory_order_acquire); }

    /** @brief 检查管道内部缓冲区及参数是否有效 */
    bool isValid(void) const { return m_valid.load(std::memory_order_acquire); }

    /** @brief 增加引用计数 */
    void addRef(void);

    /** @brief 释放引用计数, 若计数归零则触发安全析构 */
    void release(void);

    /** @brief 释放 FreeSWITCH 宿主持有的初始所有权引用 (保证只释放一次) */
    void releaseOwner(void);

    /** @brief 释放 libwebsockets wsi 回调持有的引用 (保证只释放一次) */
    void releaseWsiRef(void);

    /** @brief 析构销毁当前实例 (仅在引用计数归零时被内部调用) */
    void safe_destroy(void);

    /** @brief 向 LWS 服务线程投递异步连接请求 */
    bool connect(void);

    /** @brief 将文本消息 (如元数据或 JSON) 加入发送队列并唤醒写事件 */
    void bufferForSending(const char* text);

    /** @brief 请求关闭连接并清理资源 (线程安全) */
    void closeAndDestroy(void);

    /** @brief 获取上行音频缓冲区当前可写字节空间 */
    size_t binarySpaceAvailable(void) {
      return m_audio_buffer && m_audio_buffer_write_offset < m_audio_buffer_max_len
        ? m_audio_buffer_max_len - m_audio_buffer_write_offset : 0;
    }

    /** @brief 获取保证写入单包音频所需的最小空闲门限 */
    size_t binaryMinSpace(void) {
      return m_audio_buffer_min_freespace;
    }

    /** @brief 获取当前可写入 PCM 数据的起始指针 (已越过 LWS_PRE 头部) */
    char * binaryWritePtr(void) { 
      return (m_audio_buffer && m_audio_buffer_write_offset < m_audio_buffer_max_len)
        ? (char *) m_audio_buffer + m_audio_buffer_write_offset : nullptr;
    }

    /** @brief 推进上行音频缓冲区写指针偏移量 */
    void binaryWritePtrAdd(size_t len) {
      if (len <= binarySpaceAvailable()) m_audio_buffer_write_offset += len;
    }

    /** @brief 复位上行音频缓冲区写偏移量至 LWS_PRE 起点 */
    void binaryWritePtrReset(void) {
      m_audio_buffer_write_offset = LWS_PRE;
    }

    /** @brief 加锁保护上行音频缓冲区 */
    void lockAudioBuffer(void) {
      m_audio_mutex.lock();
    }

    /** @brief 解锁上行音频缓冲区, 若有新数据则向 LWS 投递写请求 */
    void unlockAudioBuffer(void);

    /** @brief 是否配置了 HTTP Basic Auth 认证信息 */
    bool hasBasicAuth(void) {
      return !m_username.empty() && !m_password.empty();
    }

    /** @brief 获取 Basic Auth 认证凭据 */
    void getBasicAuth(std::string& username, std::string& password) {
      username = m_username;
      password = m_password;
    }

    /** @brief 发起优雅关闭: 冲刷待发送音频后断开连接 */
    void do_graceful_shutdown();

    /** @brief 检查当前是否处于优雅关闭流程 */
    bool isGracefulShutdown(void) {
      return m_gracefulShutdown.load(std::memory_order_acquire);
    }

    /** @brief 是否启用了全双工双向音频流传输 */
    bool is_bidirectional_audio_stream() {
      return m_bidirectional_audio_stream;
    }

    /** @brief 立即排队断开连接 */
    void close();

    /* 禁止默认构造与对象拷贝 */
    AudioPipe() = delete;
    AudioPipe(const AudioPipe&) = delete;
    void operator=(const AudioPipe&) = delete;

  private:
    static std::thread serviceThread; /**< 全局 LWS 服务驱动线程 */

    /** @brief LWS 核心协议事件回调 */
    static int lws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len); 

    static struct lws_context *context;                 /**< 全局 LWS 运行上下文 */
    static std::string protocolName;                   /**< 协议标识名 */
    static std::mutex mutex_connects;                  /**< 待建连队列互斥锁 */
    static std::mutex mutex_disconnects;               /**< 待断开队列互斥锁 */
    static std::mutex mutex_writes;                    /**< 待写事件队列互斥锁 */
    static std::list<AudioPipe*> pendingConnects;      /**< 待建连管道列表 */
    static std::unordered_map<struct lws*, AudioPipe*> pendingConnectsByWsi; /**< wsi 索引表 */
    static std::list<AudioPipe*> pendingDisconnects;   /**< 待断开管道列表 */
    static std::list<AudioPipe*> pendingWrites;        /**< 待写通知管道列表 */
    static log_emit_function logger;                   /**< 日志回调 */

    static std::mutex mapMutex;                        /**< 全局生命周期与启动锁 */
    static std::mutex instancesMutex;                  /**< 全量活动实例注册表保护锁 */
    static std::unordered_set<AudioPipe*> instances;   /**< 全量活动实例集合 */
    static std::atomic<bool> stopFlag;                 /**< 服务线程停止标记 */
    static std::atomic<bool> shuttingDown;             /**< 模块正在卸载标记 */

    static AudioPipe* findAndRemovePendingConnect(struct lws *wsi);
    static AudioPipe* findPendingConnect(struct lws *wsi);
    static bool addPendingConnect(AudioPipe* ap);
    static void addPendingDisconnect(AudioPipe* ap);
    static void addPendingWrite(AudioPipe* ap);
    static void removeFromPendingLists(AudioPipe* ap);
    static void processPendingConnects(lws_per_vhost_data *vhd);
    static void processPendingDisconnects(lws_per_vhost_data *vhd);
    static void processPendingWrites(void);
    
    /** @brief 在 LWS 服务线程内执行底层 lws_client_connect_via_info */
    bool connect_client(struct lws_per_vhost_data *vhd);

    std::atomic<LwsState_t> m_state;       /**< 原子连接状态机 */
    std::string m_uuid;                     /**< 会话 UUID */
    std::string m_host;                     /**< 服务器主机名 */
    std::string m_bugname;                  /**< Media bug 名称 */
    uint64_t m_generation;                  /**< 通话代际编号 */
    unsigned int m_port;                    /**< 服务器端口 */
    std::string m_path;                     /**< 请求 URL 路径及参数 */
    std::list<std::string> m_metadata_list; /**< 待发送的文本元数据消息队列 */
    std::mutex m_text_mutex;                /**< 文本消息队列锁 */
    std::mutex m_audio_mutex;               /**< 上行音频缓冲锁 */
    int m_sslFlags;                         /**< TLS 配置掩码 */
    struct lws *m_wsi;                      /**< 底层 WebSocket 句柄 */
    uint8_t *m_audio_buffer;                /**< 上行音频线性缓冲 (首部预留 LWS_PRE) */
    size_t m_audio_buffer_max_len;          /**< 缓冲总容量 */
    size_t m_audio_buffer_write_offset;     /**< 当前写入偏移量 (>= LWS_PRE) */
    size_t m_audio_buffer_min_freespace;    /**< 允许写入的最小空闲余量 */
    uint8_t* m_recv_buf;                    /**< 下行文本碎片重组动态缓冲 */
    uint8_t* m_recv_buf_ptr;                /**< 下行文本写入游标 */
    size_t m_recv_buf_len;                  /**< 下行文本缓冲当前容量 */
    struct lws_per_vhost_data* m_vhd;       /**< 所属 vhost 上下文 */
    notifyHandler_t m_callback;             /**< 业务层事件上报回调 */
    log_emit_function m_logger;             /**< 日志回调 */
    std::string m_username;                 /**< Basic Auth 用户名 */
    std::string m_password;                 /**< Basic Auth 密码 */
    std::atomic<bool> m_gracefulShutdown;   /**< 是否已标记优雅关闭 */
    std::atomic<bool> m_delete_on_close;    /**< 关闭后自动删除标记 */
    std::atomic<bool> m_owner_released;     /**< 宿主引用是否已释放 */
    std::atomic<bool> m_valid;              /**< 对象初始化有效标记 */
    std::atomic<bool> m_disconnect_pending; /**< 断开请求排队中去重标记 */
    std::atomic<bool> m_wsi_ref;            /**< wsi 持有引用标记 */
    std::atomic<unsigned int> m_refCount;   /**< 原子引用计数器 */
    std::atomic<bool> m_write_pending;      /**< 写通知排队中去重标记 */
    bool m_bidirectional_audio_stream;      /**< 是否启用全双工下行二进制音频流 */
  };

} // namespace drachtio

#endif /* __AUDIO_PIPE_HPP__ */
