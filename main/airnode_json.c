/*
 * airnode_json.c - 轻量 JSON key-value 解析器
 *
 * 与原 STM32 simple_json 行为一致，不依赖堆/动态 JSON 库。
 */
#include <string.h>
#include <stdio.h>

#include "airnode_json.h"

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static const char *find_value(const char *json, const char *key)
{
    if (!json || !key) {
        return NULL;
    }

    int keylen = (int)strlen(key);

    while (*json) {
        if (*json != '"') {
            json++;
            continue;
        }

        json++; /* 跳过左引号 */

        if (strncmp(json, key, (size_t)keylen) == 0 && json[keylen] == '"') {
            json += keylen;

            if (*json != '"') {
                return NULL;
            }
            json++; /* 跳过右引号 */

            json = skip_space(json);
            if (*json != ':') {
                return NULL;
            }
            json++; /* 跳过冒号 */
            json = skip_space(json);

            return json;
        }

        json = strchr(json, '"');
        if (!json) {
            return NULL;
        }
        json++; /* 跳到下一个双引号后 */
    }

    return NULL;
}

int airnode_json_get_string(const char *json, const char *key,
                            char *value, int maxlen)
{
    if (!value || maxlen <= 0) {
        return -4;
    }

    const char *v = find_value(json, key);
    if (!v) {
        return -1;
    }

    if (*v != '"') {
        return -3;
    }

    v++; /* 跳过左引号 */

    int i = 0;
    while (*v && *v != '"' && i < maxlen - 1) {
        if (*v == '\\') {
            v++;
            if (!*v) {
                break;
            }
            switch (*v) {
            case 'n':  value[i++] = '\n'; break;
            case 't':  value[i++] = '\t'; break;
            case 'r':  value[i++] = '\r'; break;
            case '\\': value[i++] = '\\'; break;
            case '"':  value[i++] = '"';  break;
            default:   value[i++] = *v;   break;
            }
            v++;
        } else {
            value[i++] = *v++;
        }
    }

    if (i >= maxlen - 1) {
        return -2;
    }
    if (*v != '"') {
        return -3;
    }

    value[i] = '\0';
    return 0;
}

int airnode_json_get_int(const char *json, const char *key, int *value)
{
    if (!value) {
        return -1;
    }

    const char *v = find_value(json, key);
    if (!v) {
        return -1;
    }

    /* 兼容 {"key":"123"} 这类带引号的数值 */
    if (*v == '"') {
        v++;
    }

    int sign = 1;
    if (*v == '-') {
        sign = -1;
        v++;
    } else if (*v == '+') {
        v++;
    }

    if (*v < '0' || *v > '9') {
        return -2;
    }

    int num = 0;
    while (*v >= '0' && *v <= '9') {
        num = num * 10 + (*v - '0');
        v++;
    }

    *value = num * sign;
    return 0;
}
