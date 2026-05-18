#include "parser.hpp"
#include <switch.h>

cJSON* parse_json(switch_core_session_t* session, const std::string& data, std::string& type) {
  cJSON* json = NULL;
  const char *szType = NULL;
  json = cJSON_Parse(data.c_str());
  if (!json) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "解析 - 无法将传入消息解析为 JSON: %s\n", data.c_str());
    return NULL;
  }

  szType = cJSON_GetObjectCstr(json, "type");
  if (szType) {
    type.assign(szType);
  }
  else {
    type.assign("json");
  }
  return json;
}
