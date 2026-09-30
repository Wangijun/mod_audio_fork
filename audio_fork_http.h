/**
 * @file audio_fork_http.h
 * @brief HTTP/HTTPS Chunked 异步拉流下行虚拟文件驱动头文件
 *
 * 本驱动负责将 FreeSWITCH 的原生播放命令挂接到远端 HTTP TTS 流式服务端,
 * 支持以 chunked 编码形式异步拉取裸 S16LE PCM 数据并流式混音播放.
 *
 * 播放 URL 格式:
 *   audio_fork://http://host:port/path[?rate=16000&channels=1&watchdog=3000]
 *   audio_fork://https://host:port/path[?...]
 *
 * 核心设计:
 * 1. 异步解耦: 独立的后台 switch_thread_t 运行 libcurl 传输循环, 不阻塞 FreeSWITCH 混音时钟;
 * 2. 背压与同步: 基于 switch_buffer_t 环形缓冲与 switch_thread_cond_t 条件变量, 支持生产/消费双向背压;
 * 3. 即时解阻塞: 通道挂断或收到打断请求时, 通过 shutdown(curlfd, SHUT_RDWR) 强行中断套接字阻塞, 实现毫秒级退出;
 * 4. 起播缓冲门控: 预缓冲达到 http-prebuffer-ms (默认 200ms) 后开闸起播; 针对慢速流提供 500ms 降级门控;
 * 5. 欠载容错与统计: 缓冲瞬时读空时平滑填补静音帧, 并在播放结束时上报缓冲欠载统计.
 */

#ifndef __AUDIO_FORK_HTTP_H__
#define __AUDIO_FORK_HTTP_H__

#include <switch.h>
#include <switch_curl.h>
#include "mod_audio_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认连续欠载 150 帧 (3000ms) 触发看门狗强退 */
#define SILENCE_WATCHDOG_DEFAULT_FRAMES 150
/* 裸 PCM16 动态环形池硬上限: 约 50 秒 16kHz 16-bit Mono (1.6 MB), 防爆内存且杜绝长播报丢帧 */
#define MAX_AUDIO_FORK_BUFFER_BYTES     1600000

/**
 * @brief HTTP 流式拉流播放句柄私有上下文
 *
 * 注意: 首字段 driver_type 必须是 AUDIO_FORK_DRIVER_HTTP, 供统一文件驱动接口分发识别.
 */
typedef struct audio_fork_http_ctx {
  audio_fork_driver_type_t    driver_type;        /**< 驱动类型标识: 必须位于结构体首地址 */
  switch_memory_pool_t       *pool;               /**< 专属内存池 */
  char                       *stream_url;         /**< HTTP/HTTPS 完整 URL */
  switch_buffer_t            *audio_buffer;       /**< 裸 PCM16 动态环形池 */
  switch_mutex_t             *audio_mutex;        /**< 缓冲与状态保护锁 */
  switch_thread_cond_t       *audio_cond;         /**< 写入背压与消费唤醒条件变量 */
  switch_thread_t            *read_thread;        /**< 后台 curl 异步拉流线程 */
  curl_socket_t               curlfd;             /**< 底层 socket fd (关闭时 shutdown 即刻解阻塞) */

  uint32_t                    samplerate;         /**< 目标采样率 (默认 16000) */
  uint32_t                    channels;           /**< 目标声道数 (默认 1) */
  uint32_t                    prebuffer_bytes;    /**< 起播所需预缓冲字节门限 */

  switch_atomic_t             abort_requested;    /**< 打断/关闭标记: 1=立即终止拉流 */
  switch_atomic_t             eof;                /**< curl 拉流是否正常结束 (EOF) */
  switch_atomic_t             err;                /**< 是否发生网络/HTTP 错误 */
  long                        http_response_code; /**< HTTP 响应状态码 (如 200) */

  uint32_t                    silence_frames;     /**< 连续静音欠载帧计数 (看门狗) */
  uint32_t                    underrun_frames;    /**< 播放期间缓冲读空的总次数 */
  uint64_t                    underrun_samples;   /**< 播放期间缓冲读空的总采样数 */
  uint32_t                    max_silence_frames; /**< 静音看门狗最大帧阈值 */
} audio_fork_http_ctx_t;

/**
 * @brief 打开 HTTP Chunked 流式播放句柄并启动拉流工作线程
 * @param handle FreeSWITCH 文件句柄指针
 * @param path 资源定位路径: audio_fork://http://... 或 audio_fork://https://...
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t audio_fork_http_file_open(switch_file_handle_t *handle, const char *path);

/**
 * @brief 关闭 HTTP 流式播放句柄, 终止后台拉流线程并清理资源
 * @param handle FreeSWITCH 文件句柄指针
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t audio_fork_http_file_close(switch_file_handle_t *handle);

/**
 * @brief 从 HTTP 环形缓冲中读取解码后的 S16LE PCM 采样点
 * @param handle FreeSWITCH 文件句柄指针
 * @param data 接收 PCM 数据的目标缓冲区
 * @param len 输入为期望读取的采样点数, 输出为实际填充的采样点数
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, EOF 或出错返回 SWITCH_STATUS_FALSE
 */
switch_status_t audio_fork_http_file_read(switch_file_handle_t *handle, void *data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_FORK_HTTP_H__ */
