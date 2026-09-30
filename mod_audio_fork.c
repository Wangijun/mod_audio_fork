/**
 * @file mod_audio_fork.c
 * @brief FreeSWITCH 音频分叉与双向流式传输模块 (mod_audio_fork) 核心实现
 *
 * 本模块实现两项核心服务:
 * 1. API 接口: uuid_audio_fork
 *    提供 start / stop / pause / resume / graceful-shutdown / send_text 控制命令,
 *    实现通话音频实时单向/双向分叉上送至远程 WebSocket 服务端, 并支持双向 JSON 信令控制.
 * 2. 虚拟文件接口: switch_file_interface ("audio_fork")
 *    提供双模统一流式播放虚拟文件驱动:
 *    - audio_fork://http[s]://... -> 路由至 HTTP Chunked 异步拉流驱动 (audio_fork_http)
 *    - audio_fork://<uuid>        -> 路由至 WebSocket 全双工内存桥驱动 (audio_fork_ws)
 */

#include "mod_audio_fork.h"
#include "lws_glue.h"
#include "audio_fork_http.h"
#include "audio_fork_ws.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

/** 下行 WebSocket 播放起播预缓冲毫秒数 (默认 200 ms) */
uint32_t audio_fork_ws_prebuffer_ms = 200;

/** 下行 HTTP 播放起播预缓冲毫秒数 (默认 200 ms) */
uint32_t audio_fork_http_prebuffer_ms = 200;

/**
 * @brief 从 autoload_configs/audio_fork.conf.xml 加载模块运行配置
 *
 * 支持配置项:
 * - ws-prebuffer-ms: 下行 WebSocket 播放起播缓冲门限 (毫秒, 范围 1~5000, 默认 200)
 * - http-prebuffer-ms: 下行 HTTP Chunked 播放起播缓冲门限 (毫秒, 范围 1~5000, 默认 200)
 *
 * 若配置文件不存在, 自动回退使用内置默认值 200 ms 并正常启动.
 *
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 解析非法返回 SWITCH_STATUS_TERM
 */
static switch_status_t audio_fork_load_config(void) {
    switch_xml_t xml, cfg, settings, param;
    char path[1024];
    audio_fork_ws_prebuffer_ms = 200;
    audio_fork_http_prebuffer_ms = 200;
    switch_snprintf(path, sizeof(path), "%s%sautoload_configs%saudio_fork.conf.xml",
                    SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, SWITCH_PATH_SEPARATOR);
    if (access(path, F_OK) != 0 && errno == ENOENT) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "mod_audio_fork: 未找到 %s, HTTP 与 WebSocket 预缓冲均使用默认值 200 ms\n", path);
        return SWITCH_STATUS_SUCCESS;
    }
    if (!(xml = switch_xml_open_cfg("audio_fork.conf", &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "mod_audio_fork: 无法读取 audio_fork.conf.xml\n");
        return SWITCH_STATUS_TERM;
    }
    const char *value = "";
    const char *invalid_name = "";
    if ((settings = switch_xml_child(cfg, "settings"))) {
        for (param = switch_xml_child(settings, "param"); param; param = param->next) {
            const char *name = switch_xml_attr_soft(param, "name");
            value = switch_xml_attr_soft(param, "value");
            if (!strcmp(name, "ws-prebuffer-ms") || !strcmp(name, "http-prebuffer-ms")) {
                invalid_name = name;
                char *end = NULL;
                unsigned long parsed;
                if (!*value || *value == '-' || *value == '+') goto invalid;
                for (const char *p = value; *p; ++p) {
                    if (*p < '0' || *p > '9') goto invalid;
                }
                errno = 0;
                parsed = strtoul(value, &end, 10);
                if (errno == ERANGE || *end || parsed < 1 || parsed > 5000) goto invalid;
                if (!strcmp(name, "ws-prebuffer-ms")) {
                    audio_fork_ws_prebuffer_ms = (uint32_t)parsed;
                } else {
                    audio_fork_http_prebuffer_ms = (uint32_t)parsed;
                }
            }
        }
    }
    switch_xml_free(xml);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                      "mod_audio_fork: HTTP 预缓冲 %u ms, WebSocket 预缓冲 %u ms\n",
                      audio_fork_http_prebuffer_ms, audio_fork_ws_prebuffer_ms);
    return SWITCH_STATUS_SUCCESS;
invalid:
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "mod_audio_fork: %s 无效: %s(允许 1-5000)\n", invalid_name, value);
    switch_xml_free(xml);
    return SWITCH_STATUS_TERM;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_fork_runtime);

/* ========================================================================= */
/* SWITCH_FILE_INTERFACE 双模统一分发路由                                   */
/* ========================================================================= */

/**
 * @brief 虚拟音频文件打开入口 (switch_file_interface->file_open)
 *
 * 剥离可选的 "audio_fork://" 前缀后, 执行路径分发路由:
 * 1. 若以 "http://" 或 "https://" 开头: 路由至 audio_fork_http_file_open (HTTP Chunked 驱动);
 * 2. 否则: 视为 UUID 或 bugname, 路由至 audio_fork_ws_file_open (WebSocket 内存桥驱动).
 *
 * @param handle FreeSWITCH 音频文件句柄
 * @param path 目标播放 URI 路径
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回错误码
 */
static switch_status_t audio_fork_file_open(switch_file_handle_t *handle, const char *path) {
	if (!handle || !path) return SWITCH_STATUS_FALSE;

	const char *p = path;
	if (!strncasecmp(p, "audio_fork://", 13)) {
		p += 13;
	}

	/* 模式 A: 以 http:// 或 https:// 开头, 路由至 HTTP Chunked 异步拉流 */
	if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
		return audio_fork_http_file_open(handle, path);
	}

	/* 模式 B: 以 UUID / Session 寻址, 路由至 WebSocket 全双工内存桥 */
	return audio_fork_ws_file_open(handle, path);
}

/**
 * @brief 虚拟音频文件读取入口 (switch_file_interface->file_read)
 *
 * 根据句柄 private_info 首部的 audio_fork_driver_type_t Magic Tag 派发:
 * - AUDIO_FORK_DRIVER_HTTP -> audio_fork_http_file_read
 * - AUDIO_FORK_DRIVER_WS   -> audio_fork_ws_file_read
 *
 * @param handle FreeSWITCH 音频文件句柄
 * @param data 接收 PCM 采样的目标缓冲区
 * @param len 期望读取与实际读出的采样数
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 流结束返回 SWITCH_STATUS_FALSE
 */
static switch_status_t audio_fork_file_read(switch_file_handle_t *handle, void *data, size_t *len) {
	if (!handle || !handle->private_info) return SWITCH_STATUS_FALSE;

	audio_fork_driver_type_t *tag = (audio_fork_driver_type_t *)handle->private_info;
	if (*tag == AUDIO_FORK_DRIVER_HTTP) {
		return audio_fork_http_file_read(handle, data, len);
	} else if (*tag == AUDIO_FORK_DRIVER_WS) {
		return audio_fork_ws_file_read(handle, data, len);
	}

	return SWITCH_STATUS_FALSE;
}

/**
 * @brief 虚拟音频文件关闭入口 (switch_file_interface->file_close)
 *
 * 根据句柄 private_info 首部的 audio_fork_driver_type_t Magic Tag 派发关闭:
 * - AUDIO_FORK_DRIVER_HTTP -> audio_fork_http_file_close
 * - AUDIO_FORK_DRIVER_WS   -> audio_fork_ws_file_close
 *
 * @param handle FreeSWITCH 音频文件句柄
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
static switch_status_t audio_fork_file_close(switch_file_handle_t *handle) {
	if (!handle || !handle->private_info) return SWITCH_STATUS_SUCCESS;

	audio_fork_driver_type_t *tag = (audio_fork_driver_type_t *)handle->private_info;
	if (*tag == AUDIO_FORK_DRIVER_HTTP) {
		return audio_fork_http_file_close(handle);
	} else if (*tag == AUDIO_FORK_DRIVER_WS) {
		return audio_fork_ws_file_close(handle);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load);

SWITCH_MODULE_DEFINITION(mod_audio_fork, mod_audio_fork_load, mod_audio_fork_shutdown, NULL);

static char *audio_fork_supported_formats[] = { (char *)"audio_fork", NULL };

/**
 * @brief 解析 WebSocket 采样率字符串参数
 *
 * 支持两种表达形式:
 * 1. 缩写别名: "8k", "16k", "24k", "32k", "40k", "48k", "56k", "64k" (不区分大小写);
 * 2. 整数数值: 8000 ~ 64000 范围且必须为 8000 的整数倍 (例如 8000, 16000).
 *
 * @param value 输入的采样率字符串
 * @param rate 解析成功后写入的目标采样率指针
 * @return int 成功返回 1, 格式非法或超出范围返回 0
 */
static int parse_ws_sampling_rate(const char *value, int *rate) {
	char *end = NULL;
	long parsed;
	if (!value || !rate || !*value) return 0;
	if (!strcasecmp(value, "8k")) parsed = 8000;
	else if (!strcasecmp(value, "16k")) parsed = 16000;
	else if (!strcasecmp(value, "24k")) parsed = 24000;
	else if (!strcasecmp(value, "32k")) parsed = 32000;
	else if (!strcasecmp(value, "40k")) parsed = 40000;
	else if (!strcasecmp(value, "48k")) parsed = 48000;
	else if (!strcasecmp(value, "56k")) parsed = 56000;
	else if (!strcasecmp(value, "64k")) parsed = 64000;
	else {
		if (value[0] < '0' || value[0] > '9') return 0;
		errno = 0;
		parsed = strtol(value, &end, 10);
		if (errno == ERANGE || end == value || *end != '\0') return 0;
	}
	if (parsed < 8000 || parsed > 64000 || (parsed % 8000) != 0) return 0;
	*rate = (int)parsed;
	return 1;
}

/**
 * @brief 业务事件响应处理器 (向 FreeSWITCH 事件总线抛出 SWITCH_EVENT_CUSTOM 事件)
 *
 * 构造 SWITCH_EVENT_CUSTOM 自定义事件, 绑定通道上下文变量与可选 JSON 数据载荷,
 * 并投递至 FreeSWITCH 核心事件队列, 供 ESL / 业务层订阅处理.
 *
 * @param session FreeSWITCH 通话会话
 * @param eventName 自定义事件子类名称 (如 EVENT_TRANSCRIPTION)
 * @param json 可选的 JSON 数据文本 (作为事件 Body)
 */
static void responseHandler(switch_core_session_t* session, const char * eventName, char * json) {
	switch_event_t *event;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	if (json) switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "响应处理器: 发送事件载荷: %s.\n", json);
	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
	switch_channel_event_set_data(channel, event);
	if (json) switch_event_add_body(event, "%s", json);
	switch_event_fire(&event);
}

/**
 * @brief FreeSWITCH Media Bug 音频采集主回调 (SWITCH_MEDIA_BUG_CALLBACK)
 *
 * 状态机处理:
 * - SWITCH_ABC_TYPE_INIT: 初始化 (无操作);
 * - SWITCH_ABC_TYPE_READ: 调用 fork_frame 读取当前通道最新音频帧并压入 AudioPipe;
 * - SWITCH_ABC_TYPE_CLOSE: 通道挂断或 bug 关闭时触发, 调用 fork_session_cleanup 执行优雅收尾.
 *
 * @param bug 关联的 Media Bug 句柄
 * @param user_data 会话私有数据 (private_t*)
 * @param type 回调事件动作类型 (INIT / READ / CLOSE 等)
 * @return switch_bool_t 成功返回 SWITCH_TRUE
 */
static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	(void)user_data;
	switch_bool_t ret = SWITCH_TRUE;
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	private_t* tech_pvt = (private_t *)  switch_core_media_bug_get_user_data(bug);
	if (!tech_pvt) return SWITCH_TRUE;
	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "收到 SWITCH_ABC_TYPE_CLOSE, 监听器: %s\n", tech_pvt->bugname);
			fork_session_cleanup(session, bug, NULL, 1);
		}
		break;

	case SWITCH_ABC_TYPE_READ:
		ret = fork_frame(session, bug);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return ret;
}

/**
 * @brief 启动单个通道的音频捕获与 WebSocket 异步分流
 *
 * 核心流程:
 * 1. 校验采样率、声道数、bugname 唯一性;
 * 2. 检查通道状态 (通道必须就绪且至少达到预应答 pre-answer 状态);
 * 3. 校验并获取读取编解码器 (read_codec);
 * 4. 调用 fork_session_init 分配会话私有结构与建立 AudioPipe 实例;
 * 5. 调用 switch_core_media_bug_add 将 capture_callback 挂接到通道读混音链路;
 * 6. 调用 fork_session_connect 异步发起 WebSocket 连接.
 *
 * @param session FreeSWITCH 通话会话
 * @param flags Media Bug 标志位 (如 SMBF_READ_STREAM, SMBF_WRITE_STREAM, SMBF_STEREO)
 * @param host 目标 WebSocket 主机名或 IP
 * @param port 目标 WebSocket 端口
 * @param path 目标 WebSocket 请求路径
 * @param sampling 期望的输出采样率
 * @param sslFlags SSL/TLS 配置标志
 * @param bugname 监听器唯一名称标识
 * @param metadata 建连成功后自动发送的可选初始元数据 JSON 文本
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回错误码
 */
static switch_status_t start_capture(switch_core_session_t *session,
	switch_media_bug_flag_t flags,
	char* host,
	unsigned int port,
	char* path,
	int sampling,
	int sslFlags,
	char* bugname,
	char* metadata)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_codec_t* read_codec;

	void *pUserData = NULL;
	int channels = (flags & SMBF_STEREO) ? 2 : 1;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
		"mod_audio_fork (%s): 以采样率 %d 流传输到 %s 路径 %s 端口 %d TLS: %s.\n",
		bugname, sampling, host, path, port, sslFlags ? "是" : "否");

	if (channels < 1 || channels > 2 || sampling < 8000 || sampling > 64000 || (sampling % 8000) != 0 ||
		zstr(bugname) || strlen(bugname) > MAX_BUG_LEN) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 非法采样率, 声道数或监听器名称\n");
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 监听器 %s 已经挂载!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道未就绪, 无法启动监听器 %s!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道必须达到预应答状态后才能调用 start!\n");
		return SWITCH_STATUS_FALSE;
	}

	read_codec = switch_core_session_get_read_codec(session);
	if (!read_codec || !read_codec->implementation) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道没有可用的读取 codec, 无法启动监听器 %s!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "正在调用 fork_session_init.\n");
	if (SWITCH_STATUS_FALSE == fork_session_init(session, responseHandler, read_codec->implementation->actual_samples_per_second,
		host, port, path, sampling, sslFlags, channels, bugname, metadata, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 初始化会话失败!\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "正在添加 media bug.\n");
	if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 添加 media bug 失败!\n");
		fork_data_destroy((private_t *)pUserData);
		return status;
	}

	((private_t *)pUserData)->media_bug = bug;
	switch_channel_set_private(channel, bugname, bug);

	if (fork_session_connect(&pUserData) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork 会话无法连接.\n");
		fork_session_cleanup(session, bug, NULL, 0);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "等待连接成功.\n");
	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief 停止指定 bugname 的音频分流并释放资源
 *
 * @param session FreeSWITCH 会话
 * @param bugname 监听器名称
 * @param text 关闭前发送给对端的可选文本信令
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
static switch_status_t do_stop(switch_core_session_t *session, char* bugname, char* text)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): 停止分流.\n", bugname);
		status = fork_session_cleanup(session, bug, text, 0);
	}
	return status;
}

/**
 * @brief 暂停或恢复指定 bugname 的音频采集推送
 *
 * @param session FreeSWITCH 会话
 * @param bugname 监听器名称
 * @param pause 1=暂停, 0=恢复
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
static switch_status_t do_pauseresume(switch_core_session_t *session, char* bugname, int pause)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): %s\n", bugname, pause ? "暂停" : "恢复");
	status = fork_session_pauseresume(session, bugname, pause);

	return status;
}

/**
 * @brief 触发指定 bugname 音频管道的优雅断开流程
 *
 * @param session FreeSWITCH 会话
 * @param bugname 监听器名称
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
static switch_status_t do_graceful_shutdown(switch_core_session_t *session, char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): 优雅关闭 \n", bugname);
	status = fork_session_graceful_shutdown(session, bugname);

	return status;
}

/**
 * @brief 向指定 bugname 对应的 WebSocket 连接发送文本信令
 *
 * @param session FreeSWITCH 会话
 * @param bugname 监听器名称
 * @param text 待发送的文本内容 (通常为 JSON)
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 未找到 bug 返回 SWITCH_STATUS_FALSE
 */
static switch_status_t send_text(switch_core_session_t *session, char* bugname, char* text) {
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): 正在发送文本: %s.\n", bugname, text);
		status = fork_session_send_text(session, bugname, text);
	}
	else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork (%s): 寻找文本失败: %s.\n", bugname, text);
	}
	return status;
}

#define FORK_API_SYNTAX "<uuid> [start | stop | send_text | pause | resume | graceful-shutdown] [ws/wss/http/https-url | path] [mono | mixed | stereo] [8000..64000 (8000-step)] [bugname] [metadata]"

/**
 * @brief FreeSWITCH 核心 API 导出接口: uuid_audio_fork
 *
 * 语法:
 * uuid_audio_fork <uuid> <action> [args...]
 *
 * 支持操作 (Action):
 * 1. start:
 *    uuid_audio_fork <uuid> start <ws_url> <mix_type> <sampling> [bugname] [metadata]
 *    - ws_url: 目标 WebSocket 地址 (ws:// 或 wss://)
 *    - mix_type: mono (仅读/上行), mixed (双向混音单声道), stereo (左右声道双通道)
 *    - sampling: 目标采样率 (8k..64k 且为 8000 整数倍)
 *    - bugname: 监听器标识 (默认 "audio_fork")
 *    - metadata: 初始握手 JSON 载荷
 * 2. stop:
 *    uuid_audio_fork <uuid> stop [bugname] [text]
 *    - 停止分流并关闭 WebSocket, 支持发送可选的最终文本消息
 * 3. pause / resume:
 *    uuid_audio_fork <uuid> pause|resume [bugname]
 *    - 暂停或恢复向 WebSocket 上行推送音频帧
 * 4. graceful-shutdown:
 *    uuid_audio_fork <uuid> graceful-shutdown [bugname]
 *    - 触发优雅断开: 发送待发数据后发送断开握手
 * 5. send_text:
 *    uuid_audio_fork <uuid> send_text [bugname] <json_text>
 *    - 发送自定义 JSON 文本信令. 特别说明: 由于 JSON 载荷内部往往包含空格,
 *      代码采用 switch_stristr 在原始 cmd 中定位文本起始位置, 避免被空格分词截断.
 *
 * @param cmd 原始命令行参数字符串
 * @param session 当前执行 API 的通道会话 (若在控制台执行则为 NULL)
 * @param stream 输出流句柄 (写入 +OK 或 -ERR)
 * @return switch_status_t 恒返回 SWITCH_STATUS_SUCCESS
 */
SWITCH_STANDARD_API(fork_function)
{
	char *mycmd = NULL, *argv[10] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	char *bugname = MY_BUG_NAME;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}
	if (!cmd) {
		stream->write_function(stream, "-ERR 缺少命令\n");
		return SWITCH_STATUS_SUCCESS;
	}
	if (!zstr(cmd)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_fork 命令: %s\n", cmd);
	}

	if (zstr(cmd) || argc < 2 ||
		(0 == strcmp(argv[1], "start") && argc < 5)) {

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "命令错误 %s %s %s.\n", cmd, argv[0], argv[1]);
		stream->write_function(stream, "-用法: %s\n", FORK_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			/* 子命令 1: stop 停止分流 */
			if (!strcasecmp(argv[1], "stop")) {
				char * text = NULL;
				if (argc > 3) {
					bugname = argv[2];
					text = argv[3];
				}
				else if (argc > 2) {
					if (argv[2][0] == '{' || argv[2][0] == '[') text = argv[2];
					else bugname = argv[2];
				}
				status = do_stop(lsession, bugname, text);
			}
			/* 子命令 2: pause 暂停采集推送 */
			else if (!strcasecmp(argv[1], "pause")) {
				if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 1);
			}
			/* 子命令 3: resume 恢复采集推送 */
			else if (!strcasecmp(argv[1], "resume")) {
				if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 0);
			}
			/* 子命令 4: graceful-shutdown 优雅断开 */
			else if (!strcasecmp(argv[1], "graceful-shutdown")) {
				if (argc > 2) bugname = argv[2];
				status = do_graceful_shutdown(lsession, bugname);
			}
			/* 子命令 5: send_text 发送文本信令 */
			else if (!strcasecmp(argv[1], "send_text")) {
				char * text = 0;
				if (argc < 3) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "send_text 需要指定要发送的文本参数\n");
					switch_core_session_rwunlock(lsession);
					goto done;
				}
				/* 注意: JSON 字符串内部通常包含空格 (如 {"k": "v"}),
				   switch_separate_string 会将空格截断为多个 argv 元素.
				   因此在此处直接基于原始 cmd 查找 "send_text" 偏移量以保留完整 JSON 字符串. */
				const char *p = switch_stristr("send_text", cmd);
				if (p) {
					p += 9;
					while (*p && *p == ' ') p++;
					if (argc > 3) {
						bugname = argv[2];
						const char *pText = switch_stristr(bugname, p);
						if (pText) {
							pText += strlen(bugname);
							while (*pText && *pText == ' ') pText++;
							text = (char *)pText;
						} else {
							text = argv[3];
						}
					} else {
						if (argv[2][0] == '{' || argv[2][0] == '[') {
							text = (char *)p;
						} else {
							bugname = argv[2];
						}
					}
				} else {
					if (argc > 3) {
						bugname = argv[2];
						text = argv[3];
					} else {
						if (argv[2][0] == '{' || argv[2][0] == '[') text = argv[2];
						else bugname = argv[2];
					}
				}
				status = send_text(lsession, bugname, text);
			}
			/* 子命令 6: start 开启音频捕获分流 */
			else if (!strcasecmp(argv[1], "start")) {
				switch_channel_t *channel = switch_core_session_get_channel(lsession);
				char host[MAX_WS_URL_LEN], path[MAX_PATH_LEN];
				unsigned int port;
				int sslFlags;

				int sampling = 8000;
				switch_media_bug_flag_t flags = SMBF_READ_STREAM;
				char *metadata = NULL;

				// 7 段式语法: uuid_audio_fork <uuid> start <ws_url> [mix_type] [sampling] [bugname] [metadata]
				// 针对多于 7 个参数的情况, 进行安全兼容忽略
				if (argc > 6) {
					if (argv[5][0] != '\0') bugname = argv[5];
					if (argv[6][0] != '\0') metadata = argv[6];
				}
				else if (argc > 5) {
					if (argv[5][0] == '{' || argv[5][0] == '[') metadata = argv[5];
					else if (argv[5][0] != '\0') bugname = argv[5];
				}

				if (0 == strcmp(argv[3], "mixed")) {
					flags |= SMBF_WRITE_STREAM;
				}
				else if (0 == strcmp(argv[3], "stereo")) {
					flags |= SMBF_WRITE_STREAM;
					flags |= SMBF_STEREO;
				}
				else if(0 != strcmp(argv[3], "mono")) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无效的混音类型: %s, 必须是 mono, mixed 或 stereo\n", argv[3]);
					switch_core_session_rwunlock(lsession);
					goto report_status;
				}
				if (!parse_ws_sampling_rate(argv[4], &sampling)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						"无效的采样率: %s(要求 8000..64000 且为 8000 的整数倍)\n", argv[4]);
					switch_core_session_rwunlock(lsession);
					goto report_status;
				}
				if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port, &sslFlags)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无效的 WebSocket URI: %s\n", argv[2]);
					switch_core_session_rwunlock(lsession);
					goto report_status;
				}
				status = start_capture(lsession, flags, host, port, path, sampling, sslFlags, bugname, metadata);
			}
			else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "不支持的 mod_audio_fork 命令: %s\n", argv[1]);
			}
			switch_core_session_rwunlock(lsession);
		}
		else {
			/* 若执行 stop 时通话通道已挂断释放, 视为正常幂等操作返回成功 */
			if (!strcasecmp(argv[1], "stop")) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_fork: 会话 %s 已结束或不存在, 无需重复停止\n", argv[0]);
				status = SWITCH_STATUS_SUCCESS;
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "定位会话失败 %s\n", argv[0]);
			}
		}
	}

report_status:
	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK 成功\n");
	} else {
		stream->write_function(stream, "-ERR 操作失败\n");
	}

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief 模块加载入口 (FreeSWITCH 加载 mod_audio_fork 时调用)
 *
 * 加载顺序:
 * 1. 加载 XML 预缓冲配置 (audio_fork_load_config);
 * 2. 注册 10 个自定义事件子类 (EVENT_TRANSCRIPTION, EVENT_KILL_AUDIO 等);
 * 3. 注册 uuid_audio_fork API 接口;
 * 4. 注册 switch_file_interface ("audio_fork") 双模流式文件驱动;
 * 5. 初始化 LWS 服务线程与工作线程 (fork_init).
 *
 * @param module_interface FreeSWITCH 模块接口输出指针
 * @param pool FreeSWITCH 内存池
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS, 失败返回 SWITCH_STATUS_TERM
 */
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load)
{
	switch_api_interface_t *api_interface;
	switch_file_interface_t *file_interface;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API 正在加载..\n");
	if (audio_fork_load_config() != SWITCH_STATUS_SUCCESS) return SWITCH_STATUS_TERM;

	/* 将内部结构连接到传入的空白指针 */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* 创建/注册自定义事件消息类型 */
	if (switch_event_reserve_subclass(EVENT_TRANSCRIPTION) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_TRANSFER) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_PLAY_AUDIO) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_KILL_AUDIO) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_CONNECT_SUCCESS) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_CONNECT_FAIL) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_BUFFER_OVERRUN) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_JSON) != SWITCH_STATUS_SUCCESS) {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法为 mod_audio_fork API 注册事件子类.\n");
		return SWITCH_STATUS_TERM;
	}

	SWITCH_ADD_API(api_interface, "uuid_audio_fork", "audio_fork API", fork_function, FORK_API_SYNTAX);
	switch_console_set_complete("add uuid_audio_fork start ws-url metadata");
	switch_console_set_complete("add uuid_audio_fork start ws-url");
	switch_console_set_complete("add uuid_audio_fork stop");

	/* 注册 SWITCH_FILE_INTERFACE 原生流式文件驱动接口 */
	file_interface = (switch_file_interface_t *)switch_loadable_module_create_interface(*module_interface, SWITCH_FILE_INTERFACE);
	file_interface->interface_name = modname;
	file_interface->extens = audio_fork_supported_formats;
	file_interface->file_open = audio_fork_file_open;
	file_interface->file_close = audio_fork_file_close;
	file_interface->file_read = audio_fork_file_read;

	if (fork_init() != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_audio_fork: 初始化失败\n");
		switch_event_free_subclass(EVENT_TRANSCRIPTION);
		switch_event_free_subclass(EVENT_TRANSFER);
		switch_event_free_subclass(EVENT_PLAY_AUDIO);
		switch_event_free_subclass(EVENT_KILL_AUDIO);
		switch_event_free_subclass(EVENT_DISCONNECT);
		switch_event_free_subclass(EVENT_ERROR);
		switch_event_free_subclass(EVENT_CONNECT_SUCCESS);
		switch_event_free_subclass(EVENT_CONNECT_FAIL);
		switch_event_free_subclass(EVENT_BUFFER_OVERRUN);
		switch_event_free_subclass(EVENT_JSON);
		return SWITCH_STATUS_TERM;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API 加载成功\n");

	return SWITCH_STATUS_SUCCESS;
}

/**
 * @brief 模块卸载入口 (FreeSWITCH 卸载 mod_audio_fork 时调用)
 *
 * 卸载顺序:
 * 1. 停止事件线程并释放 LWS 连接池 (fork_cleanup);
 * 2. 注销全部 10 个自定义事件子类;
 * 3. 释放模块接口资源.
 *
 * @return switch_status_t 成功返回 SWITCH_STATUS_SUCCESS
 */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown)
{
	fork_cleanup();
	switch_event_free_subclass(EVENT_TRANSCRIPTION);
	switch_event_free_subclass(EVENT_TRANSFER);
	switch_event_free_subclass(EVENT_PLAY_AUDIO);
	switch_event_free_subclass(EVENT_KILL_AUDIO);
	switch_event_free_subclass(EVENT_DISCONNECT);
	switch_event_free_subclass(EVENT_ERROR);
	switch_event_free_subclass(EVENT_CONNECT_SUCCESS);
	switch_event_free_subclass(EVENT_CONNECT_FAIL);
	switch_event_free_subclass(EVENT_BUFFER_OVERRUN);
	switch_event_free_subclass(EVENT_JSON);

	return SWITCH_STATUS_SUCCESS;
}
