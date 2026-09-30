/**
 * @file parser.cpp
 * @brief WebSocket 下行 JSON 消息解析器实现
 */

#include "parser.hpp"
#include <switch.h>

cJSON* parse_json(switch_core_session_t* session, const std::string& data, std::string& type) {
  cJSON* json = cJSON_Parse(data.c_str());
  if (!json) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                      "解析 - 无法将传入消息解析为 JSON: %s\n", data.c_str());
    return NULL;
  }

  /* 提取业务信令类型标记 (例如 speak_start, speak_done, killAudio, transcription 等) */
  const char *szType = cJSON_GetObjectCstr(json, "type");
  if (szType) {
    type.assign(szType);
  } else {
    /* 缺少 type 字段时回退为通用 json 事件类型 */
    type.assign("json");
  }

  return json;
}
