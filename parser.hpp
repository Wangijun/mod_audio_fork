/**
 * @file parser.hpp
 * @brief WebSocket 下行 JSON 消息解析器头文件
 *
 * 为 mod_audio_fork 提供对远端 ASR/TTS/AI 服务发送的控制信令与事件载荷的 JSON 反序列化支持.
 */

#ifndef __PARSER_H__
#define __PARSER_H__

#include <string>
#include <switch_json.h>

/**
 * @brief 解析 WebSocket 接收到的文本 JSON 载荷并提取信令类型
 *
 * 将传入的字符串反序列化为 cJSON 结构体, 同时提取其中的 "type" 字段 (如
 * "speak_start", "speak_done", "killAudio", "transcription", "transfer" 等).
 * 若 JSON 中未包含 "type" 属性, 则默认分类为 "json".
 *
 * @param session 当前 FreeSWITCH 通话核心会话 (用于记录会话专属日志)
 * @param data 接收到的原始文本消息字符串
 * @param[out] type 提取出的信令消息类型
 * @return cJSON* 解析成功的 JSON 对象树指针; 若解析失败返回 NULL.
 * @note 调用方必须在消费完成后显式调用 cJSON_Delete(json) 释放内存.
 */
cJSON* parse_json(switch_core_session_t* session, const std::string& data, std::string& type);

#endif /* __PARSER_H__ */
