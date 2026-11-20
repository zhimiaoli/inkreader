#pragma once
#include <stddef.h>
#include <stdint.h>

/* Host shim: software CRC-32 (reflected, poly 0xEDB88320) matching esp_rom_crc32_le. */
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
