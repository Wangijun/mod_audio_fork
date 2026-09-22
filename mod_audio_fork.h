#ifndef __MOD_FORK_H__
#define __MOD_FORK_H__

#include <switch.h>
#include <libwebsockets.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "audio_fork"
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define MAX_WS_URL_LEN (512)
#define MAX_PATH_LEN (4096)

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
  int                          buffer_overrun_notified:1;
  int                          audio_paused:1;
  int                          graceful_shutdown:1;
  char                         initialMetadata[8192];

  /* 下行 WebSocket 内存桥缓冲与状态控制 */
  switch_buffer_t             *downstream_buffer;      /* 环形音频缓冲区指针 */
  switch_mutex_t              *downstream_mutex;       /* 缓冲区保护互斥锁 */
  int                          downstream_active:1;    /* 是否有活跃的下行播放文件句柄 */
  int                          downstream_eof:1;       /* 服务端是否已发零长帧定稿 */
  int                          downstream_interrupted:1;/* 是否被打断 (killAudio / uuid_break) */
  int                          downstream_accept_audio:1;/* 是否接受下行音频写入 (killAudio 置0, speak_start 置1) */
  uint32_t                     downstream_sample_rate; /* 下行采样率 (默认 16000) */
  uint32_t                     downstream_channels;    /* 下行声道数 (默认 1) */
};

typedef struct private_data private_t;

#endif
