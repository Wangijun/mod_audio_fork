#ifndef __AUDIO_FORK_WS_H__
#define __AUDIO_FORK_WS_H__

#include <switch.h>
#include "mod_audio_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_fork_ws_ctx {
  audio_fork_driver_type_t    driver_type;        /* 驱动类型标识: 必须位于结构体首地址 */
  switch_memory_pool_t       *pool;               /* 专属内存池 */
  switch_core_session_t      *session;            /* 关联的 FreeSWITCH 会话 (持有 rwlock) */
  private_t                  *tech_pvt;           /* 关联的 media bug 私有结构体 */
  char                        uuid[MAX_SESSION_ID];/* 会话 UUID */
  char                        bugname[MAX_BUG_LEN+1];/* media bug 名称 */

  uint32_t                    samplerate;         /* 下行采样率 (默认 16000) */
  uint32_t                    channels;           /* 下行声道数 (默认 1) */
  uint32_t                    prebuffer_bytes;    /* 起播所需预缓冲字节数 (默认 200ms) */
  int                         prebuffered;        /* 是否已完成首次起播预缓冲 */

  uint32_t                    silence_frames;     /* 连续静音欠载帧计数 (看门狗) */
  uint32_t                    max_silence_frames; /* 静音看门狗最大帧阈值 (默认 150 帧 = 3000ms) */
  uint32_t                    drain_frames;       /* 尾音排空确认计数 (防吞字) */
} audio_fork_ws_ctx_t;

switch_status_t audio_fork_ws_file_open(switch_file_handle_t *handle, const char *path);
switch_status_t audio_fork_ws_file_read(switch_file_handle_t *handle, void *data, size_t *len);
switch_status_t audio_fork_ws_file_close(switch_file_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_FORK_WS_H__ */
