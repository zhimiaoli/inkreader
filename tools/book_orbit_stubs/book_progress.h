#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {
    uint8_t pct;     ///< 全书百分比 0..100
    uint16_t chapter;
} book_progress_t;
bool book_progress_load(const char *path, uint32_t file_size, book_progress_t *out);
