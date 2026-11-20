#pragma once
#include <stdio.h>
#define ESP_LOGE(tag,fmt,...) fprintf(stderr,"[%s] " fmt "\n",tag,##__VA_ARGS__)
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

static inline esp_log_level_t esp_log_level_get(const char *tag) {
    (void)tag;
    return ESP_LOG_NONE;
}

static inline void esp_log_level_set(const char *tag, esp_log_level_t level) {
    (void)tag;
    (void)level;
}
