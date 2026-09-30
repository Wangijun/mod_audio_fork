/**
 * @file audio_fork_ws.h
 * @brief WebSocket 全双工下行内存桥虚拟文件驱动头文件
 *
 * 本驱动负责将 FreeSWITCH 的原生播放命令 (如 uuid_broadcast、playback、displace_session)
 * 挂接到当前会话的 WebSocket 下行二进制音频流上, 实现极低延迟的实时下行播放.
 *
 * 播放 URL 格式:
 *   audio_fork://<uuid>[:<bugname>][?rate=16000&channels=1&watchdog=3000]
 *
 * 核心机制:
 * 1. 内存桥接: 直接读取 private_t 中的 downstream_buffer 动态环形缓冲;
 * 2. 代际隔离: 每次 speak_start 会递增 downstream_generation, 使过期播放句柄自动失效并退出, 杜绝串音;
 * 3. 起播预缓冲: 累积满 ws-prebuffer-ms (默认 200ms) 数据后方才起播, 避免卡顿爆音;
 * 4. 欠载看门狗: 缓冲读空时自动补静音帧, 连续欠载超过 watchdog 限制 (默认 3000ms) 时安全结束播放;
 * 5. 即时打断: 收到 killAudio 信令时原子清空缓冲并标记 interrupted=1, 播放器立即中断.
 */

#ifndef __AUDIO_FORK_WS_H__
#define __AUDIO_FORK_WS_H__

#include <switch.h>
#include "mod_audio_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket 下行播放文件句柄私有上下文
 *
 * 注意: 首字段 driver_type 必须是 AUDIO_FORK_DRIVER_WS, 供统一文件驱动接口分发识别.
 */
typedef struct audio_fork_ws_ctx {
  audio_fork_driver_type_t    driver_type;        /**< 驱动类型标识: 必须位于结构体首地址 */
  switch_memory_pool_t       *pool;               /**< 专属内存池 */
  switch_core_session_t      *session;            /**< 关联的 FreeSWITCH 会话 (持有读写锁) */
  private_t                  *tech_pvt;           /**< 关联的 media bug 私有结构体 */
  char                        uuid[MAX_SESSION_ID]; /**< 会话 UUID */
  char                        bugname[MAX_BUG_LEN+1]; /**< media bug 名称 */

  uint32_t                    samplerate;         /**< 下行采样率 (默认 16000) */
  uint32_t                    channels;           /**< 下行声道数 (默认 1) */
  uint32_t                    prebuffer_bytes;    /**< 起播所需预缓冲字节门限 */
  int                         prebuffered;        /**< 是否已完成首次起播预缓冲 */

  uint32_t                    silence_frames;     /**< 连续静音欠载帧计数 (看门狗) */
  uint32_t                    max_silence_frames; /**< 静音看门狗最大帧阈值 (默认 150 帧 = 3000ms) */
  uint64_t                    generation;         /**< 打开句柄时锁定的代际, 防止串音 */
} audio_fork_ws_ctx_t;

/**
 * @brief 打开 WebSocket 下行内存桥播放句柄
 * @param handle FreeSWITCH 文件句柄指针
 * @param path 资源定位路径: audio_fork://<uuid>[:<bugname>][?<params>]
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t audio_fork_ws_file_open(switch_file_handle_t *handle, const char *path);

/**
 * @brief 从 WebSocket 环形缓冲中读取解码后的 S16LE PCM 采样
 * @param handle FreeSWITCH 文件句柄指针
 * @param data 接收 PCM 数据的目标缓冲区
 * @param len 输入为期望读取的采样点数, 输出为实际填充的采样点数
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, EOF 或出错返回 SWITCH_STATUS_FALSE
 */
switch_status_t audio_fork_ws_file_read(switch_file_handle_t *handle, void *data, size_t *len);

/**
 * @brief 关闭 WebSocket 下行内存桥播放句柄并释放会话读写锁
 * @param handle FreeSWITCH 文件句柄指针
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t audio_fork_ws_file_close(switch_file_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_FORK_WS_H__ */
