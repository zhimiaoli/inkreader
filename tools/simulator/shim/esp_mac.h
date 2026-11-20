#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

/* Host shim: deterministic fake MAC derived from a fixed base address. */
typedef enum {
    ESP_MAC_WIFI_STA = 0,
    ESP_MAC_WIFI_SOFTAP,
    ESP_MAC_BT,
    ESP_MAC_ETH,
    ESP_MAC_EFI,
    ESP_MAC_WIFI_STA_EXT,
} esp_mac_type_t;

static inline esp_err_t esp_efuse_mac_get_default(uint8_t *mac) {
    static const uint8_t base[6] = {0x24, 0x6f, 0x28, 0x00, 0x11, 0x22};
    memcpy(mac, base, 6);
    return ESP_OK;
}

static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (type == ESP_MAC_WIFI_SOFTAP) mac[5] += 1;
    else if (type == ESP_MAC_BT || type == ESP_MAC_ETH) mac[5] += 2;
    return ESP_OK;
}

static inline uint64_t esp_get_mac_hash(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | mac[i];
    return v;
}
