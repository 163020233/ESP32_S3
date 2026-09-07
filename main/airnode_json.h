/*
 * airnode_json.h - 轻量 JSON key-value 解析器
 *
 * 移植自 STM32 AirNode 的 simple_json，仅支持一层 JSON 对象。
 * 足够用于：
 *   {"command":"servo_trigger","ch":0,"action":"release"}
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提取字符串 value
 * @return 0 成功；负数为失败
 */
int airnode_json_get_string(const char *json, const char *key,
                            char *value, int maxlen);

/**
 * @brief 提取整数 value
 * @return 0 成功；负数为失败
 */
int airnode_json_get_int(const char *json, const char *key, int *value);

#ifdef __cplusplus
}
#endif
