#ifndef __AUDIO_FORK_HTTP_H__
#define __AUDIO_FORK_HTTP_H__

#include <switch.h>
#include <switch_curl.h>
#include "mod_audio_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SILENCE_WATCHDOG_DEFAULT_FRAMES 150  /* 默认连续欠载 150 帧 (3000ms) 触发看门狗强退 */
#define MAX_AUDIO_FORK_BUFFER_BYTES     1600000 /* 约 50 秒 16kHz 16-bit Mono 缓冲硬上限 (防爆内存且杜绝长播报丢帧) */
#define AUDIO_FORK_PREBUFFER_DEFAULT_MS 200  /* 起播预缓冲默认 200ms (兼顾极速起播与抗抖动，杜绝欠载斩波) */

typedef struct audio_fork_http_ctx {
  audio_fork_driver_type_t    driver_type;        /* 驱动类型标识: 必须位于结构体首地址 */
  switch_memory_pool_t       *pool;              /* 专属内存池 */
  char                       *stream_url;        /* HTTP/HTTPS 完整 URL */
  switch_buffer_t            *audio_buffer;      /* 裸 PCM16 动态环形池 */
  switch_mutex_t             *audio_mutex;       /* 缓冲与状态锁 */
  switch_thread_t            *read_thread;       /* 后台 curl 异步拉流线程 */
  curl_socket_t               curlfd;            /* 底层 socket fd (关闭时 shutdown 即刻解阻塞) */

  uint32_t                    samplerate;        /* 采样率 (默认 16000) */
  uint32_t                    channels;          /* 声道数 (默认 1) */
  uint32_t                    prebuffer_bytes;   /* 起播所需预缓冲字节数 */

  volatile int                abort_requested;   /* 打断/关闭标记：1=立即终止拉流 */
  volatile int                eof;               /* curl 拉流是否正常结束 (EOF) */
  volatile int                err;               /* 是否发生网络/HTTP 错误 */
  long                        http_response_code;/* HTTP 响应状态码 (如 200) */

  uint32_t                    silence_frames;    /* 连续静音欠载帧计数 (看门狗) */
  uint32_t                    max_silence_frames;/* 静音看门狗最大帧阈值 */
} audio_fork_http_ctx_t;

switch_status_t audio_fork_http_file_open(switch_file_handle_t *handle, const char *path);
switch_status_t audio_fork_http_file_close(switch_file_handle_t *handle);
switch_status_t audio_fork_http_file_read(switch_file_handle_t *handle, void *data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
