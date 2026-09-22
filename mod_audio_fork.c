/*
 * mod_audio_fork.c -- FreeSWITCH 模块，用于通过 WebSocket 将音频分流到远程服务器
 */
#include "mod_audio_fork.h"
#include "lws_glue.h"
#include "audio_fork_http.h"
#include "audio_fork_ws.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_audio_fork_runtime);

/* ========================================================================= */
/* SWITCH_FILE_INTERFACE 双模统一分发路由                                   */
/* ========================================================================= */

static switch_status_t audio_fork_file_open(switch_file_handle_t *handle, const char *path) {
	if (!handle || !path) return SWITCH_STATUS_FALSE;

	const char *p = path;
	if (!strncasecmp(p, "audio_fork://", 13)) {
		p += 13;
	}

	/* 模式 A：以 http:// 或 https:// 开头，路由至 HTTP Chunked 异步拉流 */
	if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
		return audio_fork_http_file_open(handle, path);
	}

	/* 模式 B：以 UUID / Session 寻址，路由至 WebSocket 全双工内存桥 */
	return audio_fork_ws_file_open(handle, path);
}

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

static void responseHandler(switch_core_session_t* session, const char * eventName, char * json) {
	switch_event_t *event;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	if (json) switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "响应处理器: 发送事件载荷: %s.\n", json);
	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
	switch_channel_event_set_data(channel, event);
	if (json) switch_event_add_body(event, "%s", json);
	switch_event_fire(&event);
}

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
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "收到 SWITCH_ABC_TYPE_CLOSE，监听器: %s\n", tech_pvt->bugname);
			fork_session_cleanup(session, tech_pvt->bugname, NULL, 1);
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

	if (switch_channel_get_private(channel, bugname)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 监听器 %s 已经挂载!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道未就绪，无法启动监听器 %s!\n", bugname);
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道必须达到预应答状态后才能调用 start!\n");
		return SWITCH_STATUS_FALSE;
	}

	read_codec = switch_core_session_get_read_codec(session);
	if (!read_codec || !read_codec->implementation) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork: 通道没有可用的读取 codec，无法启动监听器 %s!\n", bugname);
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
		return status;
	}

	((private_t *)pUserData)->media_bug = bug;
	switch_channel_set_private(channel, bugname, bug);

	if (fork_session_connect(&pUserData) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "mod_audio_fork 会话无法连接.\n");
		switch_channel_set_private(channel, bugname, NULL);
		switch_core_media_bug_remove(session, &bug);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "等待连接成功.\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname, char* text)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = (switch_media_bug_t *)switch_channel_get_private(channel, bugname);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): 停止分流.\n", bugname);
		switch_channel_set_private(channel, bugname, NULL);
		fork_session_cleanup(session, bugname, text, 0);
		status = switch_core_media_bug_remove(session, &bug);
	}
	return status;
}

static switch_status_t do_pauseresume(switch_core_session_t *session, char* bugname, int pause)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): %s\n", bugname, pause ? "暂停" : "恢复");
	status = fork_session_pauseresume(session, bugname, pause);

	return status;
}

static switch_status_t do_graceful_shutdown(switch_core_session_t *session, char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "mod_audio_fork (%s): 优雅关闭 \n", bugname);
	status = fork_session_graceful_shutdown(session, bugname);

	return status;
}

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

#define FORK_API_SYNTAX "<uuid> [start | stop | send_text | pause | resume | graceful-shutdown] [wss-url | path] [mono | mixed | stereo] [8000 | 16000 | 24000 | 32000 | 64000] [bugname] [metadata]"
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
			else if (!strcasecmp(argv[1], "pause")) {
				if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 1);
			}
			else if (!strcasecmp(argv[1], "resume")) {
				if (argc > 2) bugname = argv[2];
				status = do_pauseresume(lsession, bugname, 0);
			}
			else if (!strcasecmp(argv[1], "graceful-shutdown")) {
				if (argc > 2) bugname = argv[2];
				status = do_graceful_shutdown(lsession, bugname);
			}
			else if (!strcasecmp(argv[1], "send_text")) {
				char * text = 0;
				if (argc < 3) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "send_text 需要指定要发送的文本参数\n");
					switch_core_session_rwunlock(lsession);
					goto done;
				}
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
			else if (!strcasecmp(argv[1], "start")) {
				switch_channel_t *channel = switch_core_session_get_channel(lsession);
				char host[MAX_WS_URL_LEN], path[MAX_PATH_LEN];
				unsigned int port;
				int sslFlags;

				int sampling = 8000;
				switch_media_bug_flag_t flags = SMBF_READ_STREAM;
				char *metadata = NULL;

				// 7 段式语法：uuid_audio_fork <uuid> start <ws_url> [mix_type] [sampling] [bugname] [metadata]
				// 针对多于 7 个参数的情况，进行安全兼容忽略
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
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无效的混音类型: %s，必须是 mono、mixed 或 stereo\n", argv[3]);
					switch_core_session_rwunlock(lsession);
					goto done;
				}
				if (0 == strcmp(argv[4], "16k")) {
					sampling = 16000;
				}
				else if (0 == strcmp(argv[4], "8k")) {
					sampling = 8000;
				}
				else {
					sampling = atoi(argv[4]);
				}
				if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port, &sslFlags)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无效的 WebSocket URI: %s\n", argv[2]);
					switch_core_session_rwunlock(lsession);
					goto done;
				}
				else if (sampling % 8000 != 0) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "无效的采样率: %s\n", argv[4]);
					switch_core_session_rwunlock(lsession);
					goto done;
				}
				status = start_capture(lsession, flags, host, port, path, sampling, sslFlags, bugname, metadata);
			}
			else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "不支持的 mod_audio_fork 命令: %s\n", argv[1]);
			}
			switch_core_session_rwunlock(lsession);
		}
		else {
			if (!strcasecmp(argv[1], "stop")) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "mod_audio_fork: 会话 %s 已结束或不存在，无需重复停止\n", argv[0]);
				status = SWITCH_STATUS_SUCCESS;
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "定位会话失败 %s\n", argv[0]);
			}
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK 成功\n");
	} else {
		stream->write_function(stream, "-ERR 操作失败\n");
	}

done:
	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_audio_fork_load)
{
	switch_api_interface_t *api_interface;
	switch_file_interface_t *file_interface;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API 正在加载..\n");

	/* 将内部结构连接到传入的空白指针 */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* 创建/注册自定义事件消息类型 */
	if (switch_event_reserve_subclass(EVENT_TRANSCRIPTION) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_TRANSFER) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_PLAY_AUDIO) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_KILL_AUDIO) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_ERROR) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(EVENT_DISCONNECT) != SWITCH_STATUS_SUCCESS) {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "无法为 mod_audio_fork API 注册事件子类.\n");
		return SWITCH_STATUS_TERM;
	}

	SWITCH_ADD_API(api_interface, "uuid_audio_fork", "audio_fork API", fork_function, FORK_API_SYNTAX);
	switch_console_set_complete("add uuid_audio_fork start wss-url metadata");
	switch_console_set_complete("add uuid_audio_fork start wss-url");
	switch_console_set_complete("add uuid_audio_fork stop");

	/* 注册 SWITCH_FILE_INTERFACE 原生流式文件驱动接口 */
	file_interface = (switch_file_interface_t *)switch_loadable_module_create_interface(*module_interface, SWITCH_FILE_INTERFACE);
	file_interface->interface_name = modname;
	file_interface->extens = audio_fork_supported_formats;
	file_interface->file_open = audio_fork_file_open;
	file_interface->file_close = audio_fork_file_close;
	file_interface->file_read = audio_fork_file_read;

	fork_init();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork API 加载成功\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_fork_shutdown)
{
	fork_cleanup();
	switch_event_free_subclass(EVENT_TRANSCRIPTION);
	switch_event_free_subclass(EVENT_TRANSFER);
	switch_event_free_subclass(EVENT_PLAY_AUDIO);
	switch_event_free_subclass(EVENT_KILL_AUDIO);
	switch_event_free_subclass(EVENT_DISCONNECT);
	switch_event_free_subclass(EVENT_ERROR);

	return SWITCH_STATUS_SUCCESS;
}
