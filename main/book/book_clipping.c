/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：阅读划线/剪藏存储实现。文件布局：头部（magic/version/count/text_used）+ 条目表
 *      + 追加文本区。新增走整文件重写（条目 ≤512、文本 ≤4KB/条，重写成本可控），先写
 *      .tmp 再 rename 原子提交。导出写书库根目录，书名做文件名清洗。
 * English: Reader highlight/clipping storage. Layout: header (magic/version/count/
 *      text_used) + entry table + append-only text area. Adds rewrite the whole file
 *      (≤512 entries, ≤4KB text each keeps rewrites cheap), staged through .tmp and
 *      renamed atomically. Export lands in the book root with a sanitized title.
 *
 * 冻结：条目文本按偏移按需读取；墓碑在下次重写时物理清除（已同步后）；时间戳来自
 *      time(NULL)。
 * Frozen: entry text reads on demand by offset; synced tombstones are physically dropped
 *      on the next rewrite; timestamps come from time(NULL).
 */

#include "book_clipping.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif

#ifndef BOOK_CLIPPING_STATE_DIR
#define BOOK_CLIPPING_STATE_DIR "/sdcard/.kiikoread"
#endif

#define CLIPPING_MAGIC UINT32_C(0x4B4C4331) /* "KLC1" */
#define CLIPPING_VERSION 1
#define CLIPPING_TEXT_AREA_MAX (256U * 1024U)

typedef struct {
    uint32_t magic, version, count, text_used;
} clipping_header_t;

typedef struct {
    uint32_t start_offset, end_offset, layout_signature;
    uint16_t start_page, end_page;
    uint32_t timestamp;
    uint64_t sync_revision;
    uint32_t text_offset, text_length;
    uint16_t index;
    uint16_t flags; /* bit0 pending_upload, bit1 deleted */
} clipping_entry_t;

static book_clipping_t s_entries[BOOK_CLIPPINGS_MAX];
static int s_entry_count;
static char s_current_file[128];
static char s_book_path[256];
static char* s_text_area; /* 256KB 文本工作区（设备 PSRAM / 宿主堆）。 */

// ---- 路径 / paths --------------------------------------------------------------

static uint64_t path_hash(const char* path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

bool book_clipping_file_path(const char* book_path, char* out, size_t cap) {
    if (!book_path || !book_path[0] || !out || cap < 64) return false;
    int n = snprintf(out, cap, BOOK_CLIPPING_STATE_DIR "/clippings/%016llx.bin",
                     (unsigned long long)path_hash(book_path));
    return n > 0 && (size_t)n < cap;
}

// ---- 序列化 / serialization ----------------------------------------------------

static uint16_t entry_flags(const book_clipping_t* e) {
    return (uint16_t)((e->pending_upload ? 1 : 0) | (e->deleted ? 2 : 0));
}

static void entry_unpack(const clipping_entry_t* in, book_clipping_t* out) {
    memset(out, 0, sizeof(*out));
    out->start_offset = in->start_offset;
    out->end_offset = in->end_offset;
    out->layout_signature = in->layout_signature;
    out->start_page = in->start_page;
    out->end_page = in->end_page;
    out->timestamp = in->timestamp;
    out->sync_revision = in->sync_revision;
    out->text_offset = in->text_offset;
    out->text_length = in->text_length;
    out->index = in->index;
    out->pending_upload = (in->flags & 1) != 0;
    out->deleted = (in->flags & 2) != 0;
}

static void entry_pack(const book_clipping_t* e, clipping_entry_t* out) {
    memset(out, 0, sizeof(*out));
    out->start_offset = e->start_offset;
    out->end_offset = e->end_offset;
    out->layout_signature = e->layout_signature;
    out->start_page = e->start_page;
    out->end_page = e->end_page;
    out->timestamp = e->timestamp;
    out->sync_revision = e->sync_revision;
    out->text_offset = e->text_offset;
    out->text_length = e->text_length;
    out->index = e->index;
    out->flags = entry_flags(e);
}

bool book_clipping_header_valid(uint32_t magic, uint32_t version, uint32_t count, uint32_t text_used,
                                size_t file_size) {
    if (magic != CLIPPING_MAGIC || version != CLIPPING_VERSION) return false;
    if (count > BOOK_CLIPPINGS_MAX) return false;
    if (text_used > CLIPPING_TEXT_AREA_MAX) return false;
    size_t expected =
        sizeof(clipping_header_t) + (size_t)count * sizeof(clipping_entry_t) + text_used;
    return file_size == expected;
}

// ---- 工作区 / workspace --------------------------------------------------------

/// 256KB 文本工作区：设备从 PSRAM 取，宿主走普通堆。
/// 256KB text workspace: PSRAM on device, plain heap on host.
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static char* workspace(void) {
    if (s_text_area) return s_text_area;
    s_text_area = heap_caps_malloc(CLIPPING_TEXT_AREA_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_text_area;
}
#else
static char* workspace(void) {
    if (s_text_area) return s_text_area;
    s_text_area = malloc(CLIPPING_TEXT_AREA_MAX);
    return s_text_area;
}
#endif

/// 从磁盘读文本区到工作区；返回 text_used。/ Read the on-disk text area; returns text_used.
static uint32_t load_text_area(void) {
    char* area = workspace();
    if (!area) return 0;
    FILE* file = fopen(s_current_file, "rb");
    if (!file) return 0;
    clipping_header_t header;
    uint32_t used = 0;
    if (fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        header.magic == CLIPPING_MAGIC && header.version == CLIPPING_VERSION &&
        header.text_used <= CLIPPING_TEXT_AREA_MAX) {
        if (fseek(file, (long)(sizeof(header) + (long)header.count * sizeof(clipping_entry_t)),
                  SEEK_SET) == 0)
            if (fread(area, 1, header.text_used, file) == header.text_used) used = header.text_used;
    }
    fclose(file);
    return used;
}

/// 读取指定条目文本。/ Read one entry's text.
static bool read_entry_text(const book_clipping_t* entry, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    FILE* file = fopen(s_current_file, "rb");
    if (!file) return false;
    clipping_header_t header;
    bool ok = false;
    if (fread(&header, 1, sizeof(header), file) == sizeof(header) &&
        header.magic == CLIPPING_MAGIC && header.version == CLIPPING_VERSION &&
        entry->text_offset + entry->text_length <= header.text_used) {
        long base = (long)(sizeof(header) + (long)header.count * sizeof(clipping_entry_t));
        size_t want = entry->text_length < cap - 1 ? entry->text_length : cap - 1;
        if (fseek(file, base + (long)entry->text_offset, SEEK_SET) == 0 &&
            fread(out, 1, want, file) == want)
            ok = true;
        out[want] = 0;
    }
    fclose(file);
    return ok;
}

// ---- 公开 API / public API -----------------------------------------------------

esp_err_t book_clipping_load(const char* book_path) {
    book_clipping_unload();
    if (!book_path || !book_path[0]) return ESP_ERR_INVALID_ARG;
    if (!book_clipping_file_path(book_path, s_current_file, sizeof(s_current_file)))
        return ESP_ERR_INVALID_ARG;
    snprintf(s_book_path, sizeof(s_book_path), "%s", book_path);
    FILE* file = fopen(s_current_file, "rb");
    if (!file) return ESP_OK; /* 无剪藏文件 = 空表。No file = empty table. */
    clipping_header_t header;
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header);
    if (ok) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        ok = book_clipping_header_valid(header.magic, header.version, header.count,
                                        header.text_used, (size_t)size);
    }
    uint32_t kept = 0;
    if (ok) {
        /* 尺寸探测后拨回条目表起点。Rewind to the entry table after the size probe. */
        fseek(file, (long)sizeof(header), SEEK_SET);
        for (uint32_t i = 0; i < header.count && kept < BOOK_CLIPPINGS_MAX; ++i) {
            clipping_entry_t raw;
            if (fread(&raw, 1, sizeof(raw), file) != sizeof(raw)) break;
            book_clipping_t entry;
            entry_unpack(&raw, &entry);
            if (entry.deleted && !entry.pending_upload) continue; /* 已同步墓碑物理清除。 */
            s_entries[kept++] = entry;
        }
    }
    fclose(file);
    if (!ok) return ESP_ERR_INVALID_RESPONSE;
    s_entry_count = (int)kept;
    return ESP_OK;
}

esp_err_t book_clipping_ensure_loaded(const char* book_path) {
    if (book_path && book_path[0] && s_book_path[0] && !strcmp(book_path, s_book_path))
        return ESP_OK;
    return book_clipping_load(book_path);
}

void book_clipping_unload(void) {
    s_entry_count = 0;
    s_current_file[0] = 0;
    s_book_path[0] = 0;
}

int book_clipping_count(void) { return s_entry_count; }

bool book_clipping_get(int index, book_clipping_t* out) {
    if (!out || index < 0 || index >= s_entry_count) return false;
    *out = s_entries[index];
    return true;
}

int book_clipping_text(int index, char* out, size_t cap) {
    if (!out || !cap || index < 0 || index >= s_entry_count) return -1;
    return read_entry_text(&s_entries[index], out, cap) ? (int)strlen(out) : -1;
}

/// 整文件重写并原子提交。/ Rewrite the whole file and commit atomically.
static esp_err_t commit_file(uint32_t text_used) {
    if (!s_current_file[0]) return ESP_ERR_INVALID_STATE;
    char dir[96];
    snprintf(dir, sizeof(dir), BOOK_CLIPPING_STATE_DIR "/clippings");
    if (mkdir(BOOK_CLIPPING_STATE_DIR, 0777) && errno != EEXIST) return ESP_ERR_INVALID_STATE;
    if (mkdir(dir, 0777) && errno != EEXIST) return ESP_ERR_INVALID_STATE;
    char temp[144];
    if (snprintf(temp, sizeof(temp), "%s.tmp", s_current_file) >= (int)sizeof(temp))
        return ESP_ERR_INVALID_STATE;
    FILE* file = fopen(temp, "wb");
    if (!file) return ESP_ERR_INVALID_STATE;
    clipping_header_t header = {CLIPPING_MAGIC, CLIPPING_VERSION, (uint32_t)s_entry_count, text_used};
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (int i = 0; ok && i < s_entry_count; ++i) {
        clipping_entry_t raw;
        entry_pack(&s_entries[i], &raw);
        ok = fwrite(&raw, 1, sizeof(raw), file) == sizeof(raw);
    }
    if (ok && text_used) ok = fwrite(s_text_area, 1, text_used, file) == text_used;
    if (fclose(file)) ok = false;
    if (!ok) {
        unlink(temp);
        return ESP_ERR_INVALID_STATE;
    }
    if (rename(temp, s_current_file) != 0) {
        unlink(temp);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t book_clipping_add(const char* book_path, uint32_t start_offset, uint32_t end_offset,
                            uint32_t layout_signature, int start_page, int end_page,
                            const char* text) {
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    /* 换书时先重载（新书写空表）。Reload when switching books (new book = empty table). */
    if (!s_current_file[0] || (book_path && s_book_path[0] && strcmp(book_path, s_book_path))) {
        esp_err_t err = book_clipping_load(book_path ? book_path : s_book_path);
        if (err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE) return err;
        if (err == ESP_ERR_INVALID_RESPONSE && book_path) {
            /* 损坏文件不阻塞新增：换新路径重建。A corrupt file does not block adds. */
            book_clipping_unload();
            if (!book_clipping_file_path(book_path, s_current_file, sizeof(s_current_file)))
                return ESP_ERR_INVALID_ARG;
            snprintf(s_book_path, sizeof(s_book_path), "%s", book_path);
        }
    }
    if (s_entry_count >= BOOK_CLIPPINGS_MAX) return ESP_ERR_NO_MEM;
    /* 追加去重：同区间重复选择直接忽略。/ Deduplicate identical ranges. */
    for (int i = 0; i < s_entry_count; ++i)
        if (s_entries[i].start_offset == start_offset && s_entries[i].end_offset == end_offset &&
            !s_entries[i].deleted)
            return ESP_OK;
    char* area = workspace();
    if (!area) return ESP_ERR_NO_MEM;
    uint32_t old_used = load_text_area();
    size_t want = strlen(text);
    if (want > BOOK_CLIPPING_TEXT_MAX) want = BOOK_CLIPPING_TEXT_MAX;
    /* 文本截断到 UTF-8 边界。/ Trim to a UTF-8 boundary. */
    while (want > 0 && ((unsigned char)text[want] & 0xC0) == 0x80) --want;
    if (!want) return ESP_ERR_INVALID_ARG;
    if (old_used + want > CLIPPING_TEXT_AREA_MAX) return ESP_ERR_NO_MEM;
    book_clipping_t entry = {0};
    entry.start_offset = start_offset;
    entry.end_offset = end_offset;
    entry.layout_signature = layout_signature;
    entry.start_page = (uint16_t)(start_page < 0 ? 0 : start_page);
    entry.end_page = (uint16_t)(end_page < 0 ? 0 : end_page);
    entry.timestamp = (uint32_t)time(NULL);
    entry.text_offset = old_used;
    entry.text_length = (uint32_t)want;
    entry.index = (uint16_t)s_entry_count;
    entry.pending_upload = true;
    memcpy(area + old_used, text, want);
    s_entries[s_entry_count++] = entry;
    esp_err_t err = commit_file(old_used + (uint32_t)want);
    if (err != ESP_OK) {
        /* 回滚内存表；磁盘文件保持原样。Roll the in-memory table back; disk untouched. */
        --s_entry_count;
        memset(&s_entries[s_entry_count], 0, sizeof(s_entries[s_entry_count]));
    }
    return err;
}

esp_err_t book_clipping_remove(int index) {
    if (index < 0 || index >= s_entry_count) return ESP_ERR_INVALID_ARG;
    s_entries[index].deleted = true;
    s_entries[index].pending_upload = true;
    /* 删除立即重写落盘（墓碑保留待同步）。Rewrite immediately (tombstone kept for sync). */
    char* area = workspace();
    if (!area) return ESP_ERR_INVALID_STATE;
    /* 重写前把文本区读进来，条目表照旧（含墓碑）。Reload the text area; table unchanged. */
    uint32_t used = load_text_area();
    return commit_file(used);
}

esp_err_t book_clipping_mark_synced(int index, uint64_t revision) {
    if (index < 0 || index >= s_entry_count) return ESP_ERR_INVALID_ARG;
    /* 墓碑的"同步完成"即删除已被服务器接受：从表里移除，下次压缩物理清除文本。 */
    /* A tombstone marked synced means the deletion was accepted: drop it from the table;
       the next compaction erases its text. */
    if (s_entries[index].deleted) {
        for (int i = index; i + 1 < s_entry_count; ++i) s_entries[i] = s_entries[i + 1];
        --s_entry_count;
        memset(&s_entries[s_entry_count], 0, sizeof(s_entries[s_entry_count]));
    } else {
        s_entries[index].pending_upload = false;
        s_entries[index].sync_revision = revision;
    }
    /* 全部同步完且无墓碑时物理压缩一次。Compact once everything is synced and no tombstones. */
    bool busy = false;
    for (int i = 0; i < s_entry_count; ++i)
        busy |= s_entries[i].pending_upload || s_entries[i].deleted;
    if (busy) return ESP_OK;
    char* area = workspace();
    if (!area) return ESP_OK; /* 压缩尽力而为。Best effort. */
    uint32_t used = 0;
    int kept = 0;
    for (int i = 0; i < s_entry_count; ++i) {
        char buf[BOOK_CLIPPING_TEXT_MAX];
        if (!read_entry_text(&s_entries[i], buf, sizeof(buf))) break;
        size_t n = strlen(buf);
        memcpy(area + used, buf, n);
        s_entries[i].text_offset = used;
        s_entries[i].text_length = (uint32_t)n;
        s_entries[i].index = (uint16_t)kept;
        used += (uint32_t)n;
        s_entries[kept++] = s_entries[i];
    }
    s_entry_count = kept;
    return commit_file(used);
}

int book_clipping_pending_count(void) {
    int n = 0;
    for (int i = 0; i < s_entry_count; ++i)
        if (s_entries[i].pending_upload) ++n;
    return n;
}

int book_clipping_deleted_count(void) {
    int n = 0;
    for (int i = 0; i < s_entry_count; ++i)
        if (s_entries[i].deleted) ++n;
    return n;
}

// ---- 导出 / export -------------------------------------------------------------

bool book_clipping_format_entry(const book_clipping_t* entry, const char* text,
                                const char* book_title, bool markdown, char* out, size_t cap) {
    if (!entry || !text || !out || !cap) return false;
    char stamp[32] = "";
    {
        time_t t = (time_t)entry->timestamp;
        struct tm tm_buf;
        if (localtime_r(&t, &tm_buf)) strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm_buf);
    }
    if (markdown)
        snprintf(out, cap, "> %s\n>\n> — 《%s》%s\n\n", text, book_title ? book_title : "",
                 stamp);
    else
        snprintf(out, cap, "【%s · 《%s》】\n%s\n\n", stamp[0] ? stamp : "剪藏",
                 book_title ? book_title : "", text);
    return true;
}

/// 书名转安全文件名：剔除路径分隔与控制字符，截到 48 字节（UTF-8 边界）。
/// Title to a safe filename: drop separators/control bytes, cap at 48 bytes on a UTF-8 edge.
static bool safe_title(const char* title, char* out, size_t cap) {
    if (!out || cap < 8) return false;
    size_t used = 0;
    for (const unsigned char* p = (const unsigned char*)(title && title[0] ? title : "剪藏");
         *p && used + 4 < cap; ++p) {
        unsigned char c = *p;
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == '.')
            continue;
        if (c < 0x80) {
            out[used++] = (char)c;
            continue;
        }
        /* 多字节字符整体拷贝。Copy multi-byte sequences whole. */
        size_t n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
        for (size_t k = 0; k < n && p[k]; ++k) out[used++] = (char)p[k];
        p += n - 1;
    }
    if (!used) {
        snprintf(out, cap, "剪藏");
        used = strlen(out);
    }
    out[used] = 0;
    return true;
}

esp_err_t book_clipping_export(const char* book_path, const char* book_title, bool markdown,
                               int* exported) {
    if (exported) *exported = 0;
    if (!book_path || !book_path[0]) return ESP_ERR_INVALID_ARG;
    if (!s_entry_count || !s_book_path[0] || strcmp(s_book_path, book_path)) {
        esp_err_t err = book_clipping_load(book_path);
        if (err != ESP_OK) return err;
    }
    int count = 0;
    for (int i = 0; i < s_entry_count; ++i)
        if (!s_entries[i].deleted) ++count;
    if (!count) return ESP_ERR_NOT_FOUND;
    /* 导出到书所在目录。Export next to the book. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", book_path);
    char* slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof(dir), "/sdcard");
    char name[48];
    safe_title(book_title, name, sizeof(name));
    char target[320];
    snprintf(target, sizeof(target), "%.*s/剪藏-%.*s.%s", (int)(sizeof(dir) - 96), dir,
             (int)sizeof(name), name, markdown ? "md" : "txt");
    FILE* file = fopen(target, "wb");
    if (!file) return ESP_ERR_INVALID_STATE;
    char piece[BOOK_CLIPPING_TEXT_MAX + 128];
    int written = 0;
    for (int i = 0; i < s_entry_count; ++i) {
        if (s_entries[i].deleted) continue;
        char buf[BOOK_CLIPPING_TEXT_MAX];
        if (!read_entry_text(&s_entries[i], buf, sizeof(buf))) continue;
        if (!book_clipping_format_entry(&s_entries[i], buf, book_title, markdown, piece,
                                        sizeof(piece)))
            continue;
        if (fwrite(piece, 1, strlen(piece), file) != strlen(piece)) {
            fclose(file);
            unlink(target);
            return ESP_ERR_INVALID_STATE;
        }
        ++written;
    }
    if (fclose(file)) {
        unlink(target);
        return ESP_ERR_INVALID_STATE;
    }
    if (exported) *exported = written;
    return ESP_OK;
}
