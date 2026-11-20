/* 宿主测试替身：book_opds.c 只复用文件名构造纯函数；未引用部分被 gc-sections 裁掉。
 * Host-test stub: book_opds.c reuses only the filename helper; the rest is discarded
 * via gc-sections. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define BOOK_ORBIT_PLUGIN_VERSION "1.0.0-stub"

bool book_orbit_catalog_build_filename(const char* title, const char* format, char* out, size_t cap);
