#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

#include <switch.h>
#include <libwebsockets.h>
#include <speex/speex_resampler.h>

#include <unistd.h>
#include <stdint.h>

#define MY_BUG_NAME "audio_fork"
extern uint32_t audio_fork_ws_prebuffer_ms;
extern uint32_t audio_fork_http_prebuffer_ms;
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)
#define MAX_WS_URI_LEN (MAX_WS_URL_LEN + MAX_PATH_LEN + 32)

#define EVENT_TRANSCRIPTION   "mod_audio_fork::transcription"
#define EVENT_TRANSFER        "mod_audio_fork::transfer"
#define EVENT_PLAY_AUDIO      "mod_audio_fork::play_audio"
#define EVENT_KILL_AUDIO      "mod_audio_fork::kill_audio"
#define EVENT_DISCONNECT      "mod_audio_fork::disconnect"
#define EVENT_ERROR           "mod_audio_fork::error"
#define EVENT_CONNECT_SUCCESS "mod_audio_fork::connect"
#define EVENT_CONNECT_FAIL    "mod_audio_fork::connect_failed"
#define EVENT_BUFFER_OVERRUN  "mod_audio_fork::buffer_overrun"
#define EVENT_JSON            "mod_audio_fork::json"

#define MAX_METADATA_LEN (8192)
#define AUDIO_FORK_LIFECYCLE_ACTIVE  0U
#define AUDIO_FORK_LIFECYCLE_CLOSING 1U
#define AUDIO_FORK_LIFECYCLE_CLOSED  2U

/* 统一虚拟文件接口驱动类型标识 */
typedef enum {
  AUDIO_FORK_DRIVER_HTTP = 0x48545450, /* 'HTTP' - HTTP Chunked 异步拉流驱动 */
  AUDIO_FORK_DRIVER_WS   = 0x57534F4B  /* 'WSOK' - WebSocket 全双工内存桥驱动 */
} audio_fork_driver_type_t;

struct playout {
  char *file;
  struct playout* next;
};

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, char* json);

struct private_data {
  switch_mutex_t              *mutex;
  char                         sessionId[MAX_SESSION_ID];
  char                         bugname[MAX_BUG_LEN+1];
  SpeexResamplerState         *resampler;
  responseHandler_t            responseHandler;
  void                        *media_bug;
  void                        *pAudioPipe;
  int                          ws_state;
  char                         host[MAX_WS_URL_LEN];
  unsigned int                 port;
  char                         path[MAX_PATH_LEN];
  int                          sampling;
  struct playout              *playout;
  int                          channels;
  unsigned int                 id;
  switch_atomic_t              buffer_overrun_notified;
  switch_atomic_t              audio_paused;
  switch_atomic_t              graceful_shutdown;
  switch_atomic_t              cleanup_started;
  switch_atomic_t              lifecycle_state;
  char                         initialMetadata[8192];

  /* 下行 WebSocket 内存桥缓冲与状态控制 */
  switch_buffer_t             *downstream_buffer;      /* 环形音频缓冲区指针 */
  switch_mutex_t              *downstream_mutex;       /* 缓冲区保护互斥锁 */
  switch_atomic_t              downstream_active;       /* 是否有活跃的下行播放文件句柄 */
  switch_atomic_t              downstream_eof;          /* 服务端是否已发零长帧定稿 */
  switch_atomic_t              downstream_interrupted;  /* 是否被打断 (killAudio / uuid_break) */
  switch_atomic_t              downstream_accept_audio; /* 是否接受下行音频写入 */
  switch_atomic_t              downstream_partial_error; /* 收到非整采样 PCM */
  switch_atomic_t              downstream_partial_error_notified;
  switch_atomic_t              downstream_overrun_notified;
  switch_atomic_t              downstream_handles;
  uint8_t                      downstream_partial[4];
  uint8_t                      downstream_partial_len;
  uint64_t                     downstream_generation;
  uint32_t                     downstream_sample_rate; /* 下行采样率 (默认 16000) */
  uint32_t                     downstream_channels;    /* 下行声道数 (默认 1) */
};

typedef struct private_data private_t;

#endif
