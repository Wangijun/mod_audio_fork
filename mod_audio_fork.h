#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

/**
 * @file mod_audio_fork.h
 * @brief FreeSWITCH 音频分叉与双向流式传输模块 (mod_audio_fork) 核心头文件
 *
 * 架构概述:
 * mod_audio_fork 为 FreeSWITCH 提供低延迟的双向音频流式桥接能力:
 * 1. 上行音频流 (Upstream):
 *    通过 FreeSWITCH Media Bug (SWITCH_ABC_TYPE_READ) 挂接通道通话单向/双向混音,
 *    经可选 Speex 重采样后, 通过 WebSocket 将 L16 PCM 实时推送到远端 ASR/AI 引擎;
 * 2. 下行音频流 (Downstream):
 *    支持通过 FreeSWITCH 虚拟文件接口 (switch_file_interface) 播放远端音频流:
 *    - audio_fork_ws://<bugname>: WebSocket 全双工内存桥驱动, 直接播放来自远端 WebSocket
 *      下发的二进制 PCM 流, 支持 speak_start 代际切换、killAudio 即时打断 (Barge-In) 与 speak_done 定稿;
 *    - audio_fork_http://http://...: HTTP Chunked 流式播放驱动, 基于 libcurl 异步拉流,
 *      内置起播预缓冲 (pre-buffering) 与平滑防卡顿机制;
 * 3. 业务事件通道 (Control Plane):
 *    支持收发 JSON 格式的文本信令 (如 transcription, killAudio, transfer, disconnect 等),
 *    通过 FreeSWITCH SWITCH_EVENT_CUSTOM 事件系统向 ESL/业务层通知状态.
 */

#include <switch.h>
#include <libwebsockets.h>
#include <speex/speex_resampler.h>

#include <unistd.h>
#include <stdint.h>

/** 默认 Media Bug 标识前缀 */
#define MY_BUG_NAME "audio_fork"

/** 下行 WebSocket 播放起播预缓冲毫秒数 (通过 mod_audio_fork.conf.xml 配置) */
extern uint32_t audio_fork_ws_prebuffer_ms;

/** 下行 HTTP 播放起播预缓冲毫秒数 (通过 mod_audio_fork.conf.xml 配置) */
extern uint32_t audio_fork_http_prebuffer_ms;

/* 字符串与路径长度限制常量 */
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)
#define MAX_WS_URI_LEN (MAX_WS_URL_LEN + MAX_PATH_LEN + 32)
#define MAX_METADATA_LEN (8192)

/**
 * @name FreeSWITCH 自定义事件标识 (Custom Event Subclasses)
 * @{
 */
#define EVENT_TRANSCRIPTION   "mod_audio_fork::transcription"   /**< ASR 转写识别结果 */
#define EVENT_TRANSFER        "mod_audio_fork::transfer"        /**< 呼叫转接控制信令 */
#define EVENT_PLAY_AUDIO      "mod_audio_fork::play_audio"      /**< 远端触发播放音频事件 */
#define EVENT_KILL_AUDIO      "mod_audio_fork::kill_audio"      /**< 远端触发打断停止播放事件 (Barge-In) */
#define EVENT_DISCONNECT      "mod_audio_fork::disconnect"      /**< 远端连接断开通知 */
#define EVENT_ERROR           "mod_audio_fork::error"           /**< 业务或协议错误通知 */
#define EVENT_CONNECT_SUCCESS "mod_audio_fork::connect"         /**< WebSocket 连接建立成功 */
#define EVENT_CONNECT_FAIL    "mod_audio_fork::connect_failed"  /**< WebSocket 连接建立失败 */
#define EVENT_BUFFER_OVERRUN  "mod_audio_fork::buffer_overrun"  /**< 上行/下行环形缓冲区溢出告警 */
#define EVENT_JSON            "mod_audio_fork::json"            /**< 透传自定义 JSON 数据 */
/** @} */

/**
 * @name 通话会话生命周期状态常量
 * @{
 */
#define AUDIO_FORK_LIFECYCLE_ACTIVE  0U  /**< 活跃中, 正常读写音频与收发信令 */
#define AUDIO_FORK_LIFECYCLE_CLOSING 1U  /**< 关闭中, 正在等待积压数据排干并注销 Media Bug */
#define AUDIO_FORK_LIFECYCLE_CLOSED  2U  /**< 已彻底关闭, 底层资源已释放 */
/** @} */

/**
 * @brief 统一虚拟文件接口驱动类型标识 (Magic Tag)
 *
 * 存储在文件句柄私有结构首部, 用于在 audio_fork_file_read/close 时
 * 安全区分当前文件句柄属于 HTTP 驱动还是 WebSocket 内存桥驱动.
 */
typedef enum {
  AUDIO_FORK_DRIVER_HTTP = 0x48545450, /**< 'HTTP' - HTTP Chunked 异步拉流驱动 */
  AUDIO_FORK_DRIVER_WS   = 0x57534F4B  /**< 'WSOK' - WebSocket 全双工内存桥驱动 */
} audio_fork_driver_type_t;

/**
 * @brief 远端触发播放文件链表节点 (历史保留结构)
 */
struct playout {
  char *file;             /**< 待播放的文件路径或 URI */
  struct playout* next;   /**< 链表下一节点指针 */
};

/**
 * @brief 响应事件分发回调函数指针类型
 *
 * @param session FreeSWITCH 通话会话
 * @param eventName 自定义事件子类名称 (如 EVENT_TRANSCRIPTION)
 * @param json 携带的 JSON 字符串载荷 (如有)
 */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, char* json);

/**
 * @brief mod_audio_fork 单个通话会话的核心私有数据结构体
 */
struct private_data {
  switch_mutex_t              *mutex;                   /**< 会话级保护互斥锁 (保护 AudioPipe 与通道生命周期) */
  char                         sessionId[MAX_SESSION_ID]; /**< 关联的 FreeSWITCH 会话 UUID 字符串 */
  char                         bugname[MAX_BUG_LEN+1];  /**< 当前分叉绑定的 Media Bug 名称标识 */
  SpeexResamplerState         *resampler;               /**< Speex 重采样器 (当通道物理采样率与目标采样率不一致时创建) */
  responseHandler_t            responseHandler;         /**< 事件分发回调函数 */
  void                        *media_bug;               /**< 关联的 switch_media_bug_t 指针 */
  void                        *pAudioPipe;              /**< 关联的 drachtio::AudioPipe 实例指针 */
  int                          ws_state;                /**< WebSocket 连接状态码 */
  char                         host[MAX_WS_URL_LEN];    /**< 目标 WebSocket 服务端主机名或 IP */
  unsigned int                 port;                    /**< 目标 WebSocket 服务端端口 */
  char                         path[MAX_PATH_LEN];      /**< 目标 WebSocket 请求路径与查询参数 */
  int                          sampling;                /**< 目标输出音频采样率 (如 8000, 16000) */
  struct playout              *playout;                 /**< 播放链表头指针 */
  int                          channels;                /**< 音频声道数 (1=单声道, 2=双声道) */
  unsigned int                 id;                      /**< 会话全局自增序号 (代际标识) */
  switch_atomic_t              buffer_overrun_notified; /**< 上行缓冲区溢出是否已向 ESL 触发过通知 (原子标志) */
  switch_atomic_t              audio_paused;            /**< 音频分叉是否处于暂停状态 (原子标志: 1=暂停, 0=正常) */
  switch_atomic_t              graceful_shutdown;       /**< 是否处于优雅断开流程 (原子标志) */
  switch_atomic_t              cleanup_started;         /**< 是否已进入清理流程, 杜绝重复释放 (原子标志) */
  switch_atomic_t              lifecycle_state;         /**< 通话生命周期状态 (AUDIO_FORK_LIFECYCLE_*) */
  char                         initialMetadata[MAX_METADATA_LEN]; /**< WebSocket 建连成功后自动发送的首包元数据 JSON */

  /* 下行 WebSocket 内存桥缓冲与状态控制 (全双工双向交互) */
  switch_buffer_t             *downstream_buffer;       /**< 下行环形音频缓冲区指针 (最大 2 MiB) */
  switch_mutex_t              *downstream_mutex;        /**< 下行缓冲区保护互斥锁 */
  switch_atomic_t              downstream_active;        /**< 下行播放文件句柄是否活跃 (原子标志) */
  switch_atomic_t              downstream_eof;           /**< 服务端是否已下发定稿标记 (speak_done 或零长帧) */
  switch_atomic_t              downstream_interrupted;   /**< 是否被打断 (killAudio / uuid_break) */
  switch_atomic_t              downstream_accept_audio;  /**< 是否接受下行音频写入 (speak_start 开启, EOF/打断关闭) */
  switch_atomic_t              downstream_partial_error; /**< 是否收到非整采样字节错误 */
  switch_atomic_t              downstream_partial_error_notified; /**< 非整采样错误是否已输出日志通知 */
  switch_atomic_t              downstream_overrun_notified;       /**< 下行缓冲区溢出告警是否已触发通知 */
  switch_atomic_t              downstream_handles;       /**< 当前打开的下行虚拟播放句柄引用计数 */
  uint8_t                      downstream_partial[4];   /**< 下行非整采样残余字节拼包暂存区 */
  uint8_t                      downstream_partial_len;  /**< 残余暂存区当前字节长度 */
  uint64_t                     downstream_generation;   /**< 下行句子代际计数器 (speak_start 递增, 隔离旧音频) */
  uint32_t                     downstream_sample_rate;  /**< 下行音频采样率 (默认 16000) */
  uint32_t                     downstream_channels;     /**< 下行音频声道数 (默认 1) */
};

typedef struct private_data private_t;

#endif
