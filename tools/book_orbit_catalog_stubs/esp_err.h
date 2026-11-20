#pragma once
#include "cJSON.h"

/* 宿主 stub：真实 cJSON.h 不定义 esp_err_t；这里集中补齐宿主测试用到的错误码。
 * C11 允许与 cJSON stub 相同值的重复宏定义（benign redefinition）。
 * Host stub: the real cJSON.h never typedefs esp_err_t; the codes duplicate the cJSON
 * stub values, which is a benign redefinition in C11. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_HTTP_EAGAIN 0x5012
