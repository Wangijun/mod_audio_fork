/**
 * @file lws_glue.h
 * @brief FreeSWITCH 与 WebSocket 传输层 (AudioPipe) 交互粘合层头文件
 *
 * 本模块提供 C 接口, 连接 FreeSWITCH 的 Media Bug 回调与 C++ 实现的 AudioPipe 实例:
 * 1. fork_init / fork_cleanup: 模块生命周期初始化与卸载;
 * 2. parse_ws_uri: 解析 ws://、wss://、http://、https:// URI 并提取主机、端口、路径与 TLS 配置;
 * 3. fork_session_init / fork_session_cleanup: 会话级 Media Bug 私有数据 (private_t) 与 AudioPipe 管道的创建与销毁;
 * 4. fork_frame: Media Bug 拦截到上行 PCM 帧时触发, 执行 Speex 重采样并推入 AudioPipe 发送缓冲;
 * 5. fork_session_send_text / fork_session_pauseresume / fork_session_graceful_shutdown: 控制信令下发.
 */

#ifndef __LWS_GLUE_H__
#define __LWS_GLUE_H__

#include "mod_audio_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 解析 WebSocket/HTTP 服务器 URI
 *
 * 支持解析 ws://, wss://, http://, https://, 以及 IPv6 括号地址 [::1]:port.
 * 同时会检查 FreeSWITCH 通道变量以覆盖 TLS 验证策略 (如允许自签名证书).
 *
 * @param channel FreeSWITCH 通道指针 (用于读取通道专属 TLS 环境变量, 可为 NULL)
 * @param szServerUri 待解析的完整 URI 字符串
 * @param[out] host 解析出的主机名或 IP 缓冲区 (大小至少 MAX_WS_URL_LEN)
 * @param[out] path 解析出的路径及查询参数缓冲区 (大小至少 MAX_PATH_LEN)
 * @param[out] pPort 解析出的端口号
 * @param[out] pSslFlags 解析出的 TLS 配置标志位
 * @return int 成功返回 1, 格式非法返回 0
 */
int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags);

/**
 * @brief 全局初始化模块服务与事件处理工作线程
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_init(void);

/**
 * @brief 全局停止模块服务, 释放所有管道并退出服务线程
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_cleanup(void);

/**
 * @brief 为当前通话会话初始化 private_t 数据结构、重采样器及 AudioPipe 管道
 *
 * @param session FreeSWITCH 核心会话指针
 * @param responseHandler 业务事件响应回调函数
 * @param samples_per_second 当前通话通道读取编解码器的原生采样率
 * @param host 远端 WebSocket 服务器主机名
 * @param port 远端端口
 * @param path 请求路径
 * @param sampling 期望推送至远端的采样率 (8000..64000)
 * @param sslFlags TLS 标志位
 * @param channels 声道数 (1=mono, 2=stereo)
 * @param bugname 关联的 Media Bug 标识名
 * @param metadata 初始元数据 JSON 文本 (建连成功后自动第一帧发出)
 * @param[out] ppUserData 成功时输出初始化的 private_t 指针
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
  uint32_t samples_per_second, char *host, unsigned int port, char* path, int sampling, int sslFlags, int channels, 
  char *bugname, char* metadata, void **ppUserData);

/**
 * @brief 清理并销毁单个通话会话的 Media Bug 与关联的 AudioPipe
 *
 * @param session FreeSWITCH 核心会话指针
 * @param bug Media Bug 指针
 * @param text 可选的在断开前向远端发送的关闭附带文本消息 (如优雅关闭信令)
 * @param channelIsClosing 是否是在通道挂断流程中被调用 (1=挂断自动清理, 0=API 主动 stop)
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_cleanup(switch_core_session_t *session, switch_media_bug_t *bug, char* text, int channelIsClosing);

/**
 * @brief 销毁 private_t 内部的动态资源 (下行缓冲、互斥锁与 AudioPipe)
 * @param tech_pvt Media Bug 私有数据指针
 */
void fork_data_destroy(private_t *tech_pvt);

/**
 * @brief 暂停或恢复指定 Media Bug 的上行音频分流
 *
 * @param session FreeSWITCH 核心会话指针
 * @param bugname Media Bug 标识名
 * @param pause 1=暂停, 0=恢复
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause);

/**
 * @brief 向远端请求优雅关闭 (发送剩余音频后关闭连接)
 *
 * @param session FreeSWITCH 核心会话指针
 * @param bugname Media Bug 标识名
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname);

/**
 * @brief 向当前活动的 WebSocket 管道发送任意自定义 UTF-8 文本消息 (如 JSON)
 *
 * @param session FreeSWITCH 核心会话指针
 * @param bugname Media Bug 标识名
 * @param text 待发送的文本内容
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_send_text(switch_core_session_t *session, char *bugname, char* text);

/**
 * @brief 处理 Media Bug 拦截到的上行音频帧
 *
 * FreeSWITCH SWITCH_ABC_TYPE_READ 回调中被调用.
 * 读取 PCM 帧, 若采样率不匹配则经过 Speex 重采样, 然后写入 AudioPipe 环形发送缓冲区.
 *
 * @param session FreeSWITCH 核心会话指针
 * @param bug Media Bug 指针
 * @return switch_bool_t 成功返回 SWITCH_TRUE
 */
switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug);

/**
 * @brief 废弃存根函数 (向后兼容保留)
 */
switch_status_t fork_service_threads(void);

/**
 * @brief 启动 AudioPipe 异步建连流程
 *
 * @param ppUserData 指向 private_t 指针的地址
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
switch_status_t fork_session_connect(void **ppUserData);

#ifdef __cplusplus
}
#endif

#endif /* __LWS_GLUE_H__ */
