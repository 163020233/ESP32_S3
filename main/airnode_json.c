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
    const char *p = json;

    while (p && *p) {
        p = strchr(p, '"');
        if (!p) {
            return NULL;
        }
        p++; /* 跳过左引号 */

        if (strncmp(p, key, (size_t)keylen) == 0 && p[keylen] == '"') {
            const char *q = p + keylen;      /* q 指向 key 的右引号 */
            q++;                             /* 跳过右引号 */
            q = skip_space(q);
            if (*q == ':') {
                q = skip_space(q + 1);
                return q;                    /* 真正的键值对 */
            }
            /* 命中处不是键（例如某个字符串值恰好与 key 同名）：
             * 不能提前放弃，继续向后扫描真实 key */
            p = q;
            continue;
        }

        p = strchr(p, '"');
        if (p) {
            p++; /* 跳到下一个双引号后 */
        }
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
        value[maxlen - 1] = '\0';   /* 截断也必须保证调用方缓冲有结尾符 */
        return -2;
    }
    if (*v != '"') {
        value[i] = '\0';
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
    int digits = 0;
    while (*v >= '0' && *v <= '9') {
        if (digits >= 9) {           /* 上限防 int 溢出回绕成合法小值 */
            return -2;
        }
        num = num * 10 + (*v - '0');
        digits++;
        v++;
    }

    *value = num * sign;
    return 0;
}
