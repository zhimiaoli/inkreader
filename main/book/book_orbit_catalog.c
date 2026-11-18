/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BookOrbit 书库目录客户端实现。每轮请求起一个一次性工作线程：目录根、分区列表、
 *      书籍列表、详情或文件下载；快照在锁内发布，UI 通过 revision 轮询。
 * English: BookOrbit catalog client. Each request spawns a one-shot worker: catalog root,
 *      section list, book list, detail or file download; snapshots are published under a
 *      lock and the UI polls via revision.
 *
 * 冻结：凭据只读 KOReader 同步设置；下载先写 .part 再原子改名；失败不落盘；
 *      不自动下载，全部由界面显式发起。
 * Frozen: credentials read the KOReader sync settings only; downloads stage through .part
 *      and rename atomically; failures never land on the shelf; nothing downloads without
 *      an explicit UI action.
 */

#include "book_orbit_catalog.h"

// 网络/页面控制按体积编译；协议与快照时序不变。
// Size-optimize IO/page control; protocol and snapshot timing stay unchanged.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_orbit_sync.h"
#include "book_store.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

#define CATALOG_HTTP_TIMEOUT_MS 8000
#define CATALOG_DOWNLOAD_TIMEOUT_MS 30000
#define CATALOG_RESPONSE_MAX (64U * 1024U)
#define CATALOG_LIST_ITEMS_MAX BOOK_ORBIT_CATALOG_ITEMS_MAX
#define CATALOG_BOOKS_MAX BOOK_ORBIT_CATALOG_PAGE_SIZE
#define CATALOG_FILENAME_MAX 192
#define CATALOG_STACK_DEPTH 16384 /* 16KiB：JSON 解析 + TLS 下载 / JSON parsing plus TLS download */

// ---- 快照与状态 / snapshots and status -----------------------------------------

typedef enum {
    JOB_NONE = 0,
    JOB_HOME,
    JOB_SECTION,
    JOB_BOOKS,
    JOB_DETAIL,
    JOB_DOWNLOAD,
} catalog_job_t;

static TaskHandle_t s_task;
static bool s_running;
static bool s_cancel;
static catalog_job_t s_job;
static char s_job_section[32];
static int s_job_page;
static book_orbit_catalog_books_filter_t s_job_filter;
static long s_job_book_id;
static char s_job_filename[CATALOG_FILENAME_MAX];
static book_orbit_catalog_file_t s_job_file;

static book_orbit_catalog_status_t s_status = {.state = BOOK_ORBIT_CATALOG_IDLE};

static book_orbit_catalog_item_t s_items[CATALOG_LIST_ITEMS_MAX];
static int s_item_count;
static book_orbit_catalog_book_t s_books[CATALOG_BOOKS_MAX];
static int s_book_count;
static book_orbit_catalog_detail_t* s_detail;
static int s_list_page = 1;
static int s_list_total = -1;
static bool s_list_has_next;

static void publish(book_orbit_catalog_state_t state, const char* message) {
    s_status.state = state;
    s_status.busy = state == BOOK_ORBIT_CATALOG_LOADING;
    if (state != BOOK_ORBIT_CATALOG_LOADING) {
        s_status.download_total = 0;
        s_status.download_received = 0;
    }
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    ++s_status.revision;
}

// ---- 纯函数 / pure helpers -----------------------------------------------------

bool book_orbit_catalog_api_base(const char* server, char* out, size_t cap) {
    if (!server || !out || cap < 16) return false;
    char normalized[224];
    if (!book_orbit_normalize_server_url(server, normalized, sizeof(normalized))) return false;
    size_t base = strlen(normalized);
    if (base + 8 >= cap) return false;
    memcpy(out, normalized, base);
    memcpy(out + base, "/api/v1", 8);
    return true;
}

static bool uri_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

size_t book_orbit_catalog_append_param(char* url, size_t cap, const char* key, const char* value) {
    if (!url || !cap || !key || !key[0] || !value || !value[0]) return 0;
    size_t length = strlen(url);
    static const char HEX[] = "0123456789ABCDEF";
    size_t need = length + 1 + strlen(key) + 1 + strlen(value) * 3 + 1;
    if (need > cap) return 0;
    url[length++] = strchr(url, '?') ? '&' : '?';
    for (const char* p = key; *p; ++p) {
        if (!uri_unreserved((unsigned char)*p)) return 0;
        url[length++] = *p;
    }
    url[length++] = '=';
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
        if (uri_unreserved(*p)) {
            url[length++] = (char)*p;
        } else {
            url[length++] = '%';
            url[length++] = HEX[*p >> 4];
            url[length++] = HEX[*p & 15];
        }
    }
    url[length] = 0;
    return length;
}

/// 复制最多 max_bytes 字节且不截断 UTF-8 序列。/ Copy up to max_bytes without splitting UTF-8.
static void copy_utf8_bounded(char* out, size_t cap, const char* in, size_t max_bytes) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!in) return;
    size_t bytes = 0;
    const unsigned char* p = (const unsigned char*)in;
    while (*p && bytes < max_bytes && bytes + 1 < cap) {
        size_t step = *p < 0x80 ? 1 : (*p < 0xE0 ? 2 : (*p < 0xF0 ? 3 : 4));
        if (bytes + step > max_bytes || bytes + step + 1 >= cap) break;
        memcpy(out + bytes, p, step);
        bytes += step;
        p += step;
    }
    out[bytes] = 0;
}

bool book_orbit_catalog_build_filename(const char* title, const char* format, char* out, size_t cap) {
    if (!out || cap < 16) return false;
    out[0] = 0;
    static const char ILLEGAL[] = "\\/:*?\"<>|";
    char clean[96];
    size_t bytes = 0;
    const unsigned char* p = (const unsigned char*)(title ? title : "");
    while (*p && bytes + 1 < sizeof(clean)) {
        if ((unsigned char)*p <= 0x1F || (unsigned char)*p == 0x7F ||
            (bytes == 0 && (*p == ' ' || *p == '.'))) {
            ++p;
            continue;
        }
        if (strchr(ILLEGAL, *p) && *p < 0x80) {
            clean[bytes++] = '_';
            ++p;
            continue;
        }
        size_t step = *p < 0x80 ? 1 : (*p < 0xE0 ? 2 : (*p < 0xF0 ? 3 : 4));
        if (bytes + step >= sizeof(clean)) break;
        memcpy(clean + bytes, p, step);
        bytes += step;
        p += step;
    }
    while (bytes && (clean[bytes - 1] == ' ' || clean[bytes - 1] == '.')) --bytes;
    clean[bytes] = 0;
    if (!bytes) { clean[0] = 'b'; clean[1] = 'o'; clean[2] = 'o'; clean[3] = 'k'; clean[4] = 0; }
    char ext[12];
    size_t ext_bytes = 0;
    for (const char* q = format ? format : ""; *q && ext_bytes + 1 < sizeof(ext); ++q) {
        if (*q == ' ' || *q == '\t' || (unsigned char)*q <= 0x1F) continue;
        ext[ext_bytes++] = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
    }
    ext[ext_bytes] = 0;
    if (!ext_bytes) { ext[0] = 'e'; ext[1] = 'p'; ext[2] = 'u'; ext[3] = 'b'; ext[4] = 0; }
    if (snprintf(out, cap, "%s.%s", clean, ext) >= (int)cap) {
        /* 书名过长时截到扩展名能放下为止。/ Truncate the title so the extension always fits. */
        size_t keep = bytes;
        while (keep && snprintf(out, cap, "%.*s.%s", (int)keep, clean, ext) >= (int)cap) --keep;
        if (!keep) return false;
    }
    return true;
}

int book_orbit_catalog_pick_file(const book_orbit_catalog_file_t* files, int n) {
    if (!files || n <= 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(files[i].role, "primary")) return i;
    }
    return 0;
}

// ---- HTTP / HTTP ---------------------------------------------------------------

typedef struct {
    char* buf;
    size_t cap;
    size_t length;
    bool overflow;
} json_sink_t;

static esp_err_t json_event(esp_http_client_event_t* event) {
    json_sink_t* sink = (json_sink_t*)event->user_data;
    if (!sink) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (s_cancel) return ESP_FAIL;
        if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
        size_t take = (size_t)event->data_len;
        if (sink->length + take >= sink->cap) {
            take = sink->cap ? sink->cap - sink->length - 1 : 0;
            sink->overflow = true;
        }
        if (take) {
            memcpy(sink->buf + sink->length, event->data, take);
            sink->length += take;
            sink->buf[sink->length] = 0;
        }
    }
    return ESP_OK;
}

/// GET JSON：成功时 *out 为 PSRAM 缓冲（调用方 free），status 输出 HTTP 状态码。
/// GET JSON: on success *out is a PSRAM buffer (caller frees); *status receives the code.
static bool http_get_json(const char* url, char** out, int* status) {
    *out = NULL;
    *status = 0;
    if (s_cancel) return false;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return false;
    char* buf = heap_caps_malloc(CATALOG_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;
    buf[0] = 0;
    json_sink_t sink = {.buf = buf, .cap = CATALOG_RESPONSE_MAX, .length = 0, .overflow = false};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CATALOG_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = json_event,
        .user_data = &sink,
        .crt_bundle_attach = strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(buf);
        return false;
    }
    const char* user = app_settings_orbit_user();
    const char* password = app_settings_orbit_password();
    esp_http_client_set_header(client, "Accept", "application/json");
    if (user) esp_http_client_set_header(client, "x-auth-user", user);
    char userkey[33];
    if (password && book_orbit_userkey(password, userkey)) {
        esp_http_client_set_header(client, "x-auth-key", userkey);
        memset(userkey, 0, sizeof(userkey));
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "kiikoread " BOOK_ORBIT_PLUGIN_VERSION);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    bool ok = err == ESP_OK && !s_cancel && !sink.overflow && sink.length > 0;
    esp_http_client_cleanup(client);
    if (!ok) {
        heap_caps_free(buf);
        return false;
    }
    *out = buf;
    return true;
}

// ---- JSON 解析 / JSON parsing --------------------------------------------------

static const char* json_string(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static double json_number(const cJSON* obj, const char* key, double fallback) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

/// 0..1 的小数与 0..100 的百分比都归一成 0..100。/ Normalize 0..1 fractions and 0..100 percents.
static int normalize_percent(double value, bool present) {
    if (!present) return -1;
    if (value < 0) return 0;
    if (value <= 1.0) return (int)(value * 100.0 + 0.5);
    if (value > 100) return 100;
    return (int)(value + 0.5);
}

static book_orbit_catalog_section_t section_kind(const char* section) {
    if (!section || !section[0]) return BOOK_ORBIT_SECTION_NONE;
    if (!strcmp(section, "all-books")) return BOOK_ORBIT_SECTION_ALL;
    if (!strcmp(section, "continue-reading")) return BOOK_ORBIT_SECTION_CONTINUE;
    if (!strcmp(section, "recent")) return BOOK_ORBIT_SECTION_RECENT;
    if (!strcmp(section, "authors")) return BOOK_ORBIT_SECTION_AUTHORS;
    if (!strcmp(section, "series")) return BOOK_ORBIT_SECTION_SERIES;
    if (!strcmp(section, "libraries")) return BOOK_ORBIT_SECTION_LIBRARIES;
    if (!strcmp(section, "collections")) return BOOK_ORBIT_SECTION_COLLECTIONS;
    if (!strcmp(section, "smart-scopes")) return BOOK_ORBIT_SECTION_SCOPES;
    return BOOK_ORBIT_SECTION_OTHER;
}

static void publish_list_page(const cJSON* root) {
    double page = json_number(root, "page", 1);
    double total = json_number(root, "total", -1);
    const cJSON* next = cJSON_GetObjectItem(root, "hasNext");
    s_list_page = page >= 1 ? (int)page : 1;
    s_list_total = (int)total;
    s_list_has_next = cJSON_IsTrue(next);
}

static bool parse_root(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* sections = cJSON_GetObjectItem(root, "sections");
    int count = cJSON_IsArray(sections) ? cJSON_GetArraySize(sections) : 0;
    int kept = 0;
    s_item_count = 0;
    for (int i = 0; i < count && kept < CATALOG_LIST_ITEMS_MAX; ++i) {
        const cJSON* entry = cJSON_GetArrayItem(sections, i);
        if (!cJSON_IsObject(entry)) continue;
        const char* section = json_string(entry, "section");
        if (section && !strcmp(section, "search")) continue; /* 无键盘版本先不收录搜索 / No keyboard in v1 */
        book_orbit_catalog_item_t* item = &s_items[kept];
        memset(item, 0, sizeof(*item));
        item->id = (long)json_number(entry, "id", -1);
        item->count = -1;
        item->kind = section_kind(section);
        if (cJSON_IsNumber(cJSON_GetObjectItem(entry, "count"))) {
            item->count = (int)json_number(entry, "count", -1);
        }
        copy_utf8_bounded(item->title, sizeof(item->title), json_string(entry, "title"), 120);
        ++kept;
    }
    s_item_count = kept;
    s_list_page = 1;
    s_list_total = kept;
    s_list_has_next = false;
    ++s_status.revision;
    cJSON_Delete(root);
    return true;
}

static bool parse_section_list(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* items = cJSON_GetObjectItem(root, "items");
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    book_orbit_catalog_section_t kind = BOOK_ORBIT_SECTION_OTHER;
    {
        const char* section = json_string(root, "section");
        kind = section_kind(section);
    }
    s_item_count = 0;
    int kept = 0;
    for (int i = 0; i < count && kept < CATALOG_LIST_ITEMS_MAX; ++i) {
        const cJSON* entry = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(entry)) continue;
        book_orbit_catalog_item_t* item = &s_items[kept];
        memset(item, 0, sizeof(*item));
        item->kind = kind;
        item->id = (long)json_number(entry, "id", -1);
        double series = json_number(entry, "seriesId", -1);
        item->series_id = series > 0 ? (long)series : -1;
        double author = json_number(entry, "authorId", -1);
        if (author > 0) item->id = (long)author;
        item->count = cJSON_IsNumber(cJSON_GetObjectItem(entry, "count"))
                          ? (int)json_number(entry, "count", -1)
                          : -1;
        copy_utf8_bounded(item->title, sizeof(item->title), json_string(entry, "title"), 120);
        ++kept;
    }
    s_item_count = kept;
    ++s_status.revision;
    publish_list_page(root);
    cJSON_Delete(root);
    return true;
}

static bool parse_books(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* items = cJSON_GetObjectItem(root, "items");
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    s_book_count = 0;
    int kept = 0;
    for (int i = 0; i < count && kept < CATALOG_BOOKS_MAX; ++i) {
        const cJSON* entry = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(entry)) continue;
        book_orbit_catalog_book_t* book = &s_books[kept];
        memset(book, 0, sizeof(*book));
        book->id = (long)json_number(entry, "id", -1);
        book->progress = -1;
        bool has_progress = cJSON_IsNumber(cJSON_GetObjectItem(entry, "progressPercentage"));
        book->progress = normalize_percent(json_number(entry, "progressPercentage", 0), has_progress);
        copy_utf8_bounded(book->title, sizeof(book->title), json_string(entry, "title"), 150);
        copy_utf8_bounded(book->series, sizeof(book->series), json_string(entry, "seriesName"), 90);
        copy_utf8_bounded(book->read_status, sizeof(book->read_status), json_string(entry, "readStatus"), 14);
        const cJSON* authors = cJSON_GetObjectItem(entry, "authors");
        if (cJSON_IsArray(authors)) {
            int authors_count = cJSON_GetArraySize(authors);
            size_t used = 0;
            for (int a = 0; a < authors_count && a < 2; ++a) {
                const cJSON* author = cJSON_GetArrayItem(authors, a);
                if (!cJSON_IsString(author) || !author->valuestring || !author->valuestring[0]) continue;
                size_t want = strlen(author->valuestring);
                if (used && used + 2 < sizeof(book->authors)) {
                    memcpy(book->authors + used, ", ", 2);
                    used += 2;
                }
                if (used + want >= sizeof(book->authors)) want = sizeof(book->authors) - used - 1;
                if (want) {
                    memcpy(book->authors + used, author->valuestring, want);
                    used += want;
                    book->authors[used] = 0;
                }
            }
        }
        const cJSON* formats = cJSON_GetObjectItem(entry, "formats");
        if (cJSON_IsArray(formats)) {
            int formats_count = cJSON_GetArraySize(formats);
            size_t used = 0;
            for (int f = 0; f < formats_count; ++f) {
                const cJSON* format = cJSON_GetArrayItem(formats, f);
                if (!cJSON_IsString(format) || !format->valuestring || !format->valuestring[0]) continue;
                size_t want = strlen(format->valuestring);
                if (used && used + 2 < sizeof(book->formats)) {
                    memcpy(book->formats + used, ", ", 2);
                    used += 2;
                }
                if (used + want >= sizeof(book->formats)) break;
                memcpy(book->formats + used, format->valuestring, want);
                used += want;
                book->formats[used] = 0;
            }
        }
        ++kept;
    }
    s_book_count = kept;
    ++s_status.revision;
    publish_list_page(root);
    cJSON_Delete(root);
    return true;
}

static void parse_file(const cJSON* entry, book_orbit_catalog_file_t* file) {
    memset(file, 0, sizeof(*file));
    file->id = (long)json_number(entry, "id", -1);
    file->size_bytes = -1;
    bool has_size = cJSON_IsNumber(cJSON_GetObjectItem(entry, "sizeBytes"));
    if (!has_size) has_size = cJSON_IsNumber(cJSON_GetObjectItem(entry, "sourceSizeBytes"));
    if (has_size) {
        double size = json_number(entry, "sizeBytes", -1);
        if (size < 0) size = json_number(entry, "sourceSizeBytes", -1);
        file->size_bytes = (long long)size;
    }
    copy_utf8_bounded(file->format, sizeof(file->format), json_string(entry, "format"), 14);
    copy_utf8_bounded(file->role, sizeof(file->role), json_string(entry, "role"), 14);
    const char* variant = json_string(entry, "downloadVariant");
    if (variant && variant[0] && strcmp(variant, "original")) {
        copy_utf8_bounded(file->variant, sizeof(file->variant), variant, 22);
        file->has_variant = true;
    }
}

static bool parse_detail(const char* json) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    book_orbit_catalog_detail_t* detail = heap_caps_malloc(sizeof(*detail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!detail) {
        cJSON_Delete(root);
        return false;
    }
    memset(detail, 0, sizeof(*detail));
    detail->id = (long)json_number(root, "id", -1);
    detail->progress = -1;
    detail->page_count = -1;
    detail->series_index = -1;
    bool has_progress = cJSON_IsNumber(cJSON_GetObjectItem(root, "progressPercentage"));
    detail->progress = normalize_percent(json_number(root, "progressPercentage", 0), has_progress);
    if (cJSON_IsNumber(cJSON_GetObjectItem(root, "pageCount")))
        detail->page_count = (int)json_number(root, "pageCount", -1);
    double series_index = json_number(root, "seriesIndex", -1);
    if (series_index > 0) detail->series_index = (int)(series_index * 10.0 + 0.5);
    copy_utf8_bounded(detail->title, sizeof(detail->title), json_string(root, "title"), 150);
    copy_utf8_bounded(detail->subtitle, sizeof(detail->subtitle), json_string(root, "subtitle"), 90);
    copy_utf8_bounded(detail->series, sizeof(detail->series), json_string(root, "seriesName"), 90);
    copy_utf8_bounded(detail->publisher, sizeof(detail->publisher), json_string(root, "publisher"), 60);
    copy_utf8_bounded(detail->language, sizeof(detail->language), json_string(root, "language"), 20);
    copy_utf8_bounded(detail->read_status, sizeof(detail->read_status), json_string(root, "readStatus"), 14);
    const char* year = json_string(root, "publishedYear");
    const char* date = json_string(root, "publishedDate");
    copy_utf8_bounded(detail->published, sizeof(detail->published), year && year[0] ? year : date, 20);
    const cJSON* authors = cJSON_GetObjectItem(root, "authors");
    if (cJSON_IsArray(authors)) {
        int authors_count = cJSON_GetArraySize(authors);
        size_t used = 0;
        for (int a = 0; a < authors_count && a < 3; ++a) {
            const cJSON* author = cJSON_GetArrayItem(authors, a);
            if (!cJSON_IsString(author) || !author->valuestring || !author->valuestring[0]) continue;
            size_t want = strlen(author->valuestring);
            if (used && used + 2 < sizeof(detail->authors)) {
                memcpy(detail->authors + used, ", ", 2);
                used += 2;
            }
            if (used + want >= sizeof(detail->authors)) want = sizeof(detail->authors) - used - 1;
            if (want) {
                memcpy(detail->authors + used, author->valuestring, want);
                used += want;
                detail->authors[used] = 0;
            }
        }
    }
    copy_utf8_bounded(detail->description, sizeof(detail->description), json_string(root, "description"),
                      BOOK_ORBIT_CATALOG_DESCRIPTION_MAX - 1);
    const cJSON* files = cJSON_GetObjectItem(root, "files");
    int files_count = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
    for (int i = 0; i < files_count && detail->file_count < BOOK_ORBIT_CATALOG_FILES_MAX; ++i) {
        const cJSON* entry = cJSON_GetArrayItem(files, i);
        if (!cJSON_IsObject(entry)) continue;
        book_orbit_catalog_file_t* file = &detail->files[detail->file_count];
        parse_file(entry, file);
        if (file->id <= 0) continue;
        ++detail->file_count;
    }
    cJSON_Delete(root);
    if (s_detail) heap_caps_free(s_detail);
    s_detail = detail;
    ++s_status.revision;
    return true;
}

// ---- 下载 / download -----------------------------------------------------------

typedef struct {
    FILE* file;
    long long written;
    long long total;
    bool failed;
    int64_t last_activity_us;
} file_sink_t;

static esp_err_t download_event(esp_http_client_event_t* event) {
    file_sink_t* sink = (file_sink_t*)event->user_data;
    if (!sink) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED || event->event_id == HTTP_EVENT_ON_HEADER ||
        event->event_id == HTTP_EVENT_ON_DATA)
        sink->last_activity_us = esp_timer_get_time();
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        long long declared = esp_http_client_get_content_length(event->client);
        if (declared > 0) sink->total = declared;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (s_cancel) {
            sink->failed = true;
            return ESP_FAIL;
        }
        if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
        if (event->data_len > 0 &&
            fwrite(event->data, 1, (size_t)event->data_len, sink->file) != (size_t)event->data_len) {
            sink->failed = true;
            return ESP_FAIL;
        }
        sink->written += (size_t)event->data_len;
        /* 进度条只在跨过 10% 时发布一次，避免每块数据都刷屏。 */
        /* Publish progress once per 10% to keep e-ink traffic sane. */
        static unsigned last_bucket;
        static long long last_total;
        if (sink->total != last_total) {
            last_total = sink->total;
            last_bucket = 0;
        }
        unsigned bucket = sink->total ? (unsigned)((uint64_t)sink->written * 10u / (uint64_t)sink->total) : 0;
        if (bucket != last_bucket) {
            last_bucket = bucket;
            s_status.download_received = sink->written;
            if (sink->total > 0) s_status.download_total = sink->total;
            ++s_status.revision;
        }
    }
    return ESP_OK;
}

static esp_err_t download_perform(esp_http_client_handle_t client, file_sink_t* sink) {
    for (;;) {
        if (s_cancel) return ESP_ERR_INVALID_STATE;
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) return err;
        if (esp_timer_get_time() - sink->last_activity_us >= (int64_t)CATALOG_DOWNLOAD_TIMEOUT_MS * 1000)
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/// 已存在时尝试 name (2)、name (3)。/ Try "name (2)", "name (3)" when the target exists.
static bool pick_target(const char* root, const char* name, char* out, size_t cap) {
    if (snprintf(out, cap, "%s/%s", root, name) >= (int)cap) return false;
    struct stat st;
    if (stat(out, &st) != 0) return true;
    const char* dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    const char* ext = dot ? dot : "";
    for (int i = 2; i <= 9; ++i) {
        if (snprintf(out, cap, "%s/%.*s (%d)%s", root, (int)base, name, i, ext) >= (int)cap) return false;
        if (stat(out, &st) != 0) return true;
    }
    return false;
}

static bool download_file(const char* url, const char* file_name) {
    book_store_root_t root;
    if (book_store_upload_root(&root) != ESP_OK) {
        publish(BOOK_ORBIT_CATALOG_FAILED, "没有可用的存储");
        return false;
    }
    char target[BOOK_STORE_PATH_MAX + CATALOG_FILENAME_MAX];
    if (!pick_target(root.path, file_name, target, sizeof(target))) {
        publish(BOOK_ORBIT_CATALOG_FAILED, "文件名过长");
        return false;
    }
    char part[BOOK_STORE_PATH_MAX + CATALOG_FILENAME_MAX];
    if (snprintf(part, sizeof(part), "%s/.orbit.part", root.path) >= (int)sizeof(part)) {
        publish(BOOK_ORBIT_CATALOG_FAILED, "下载缓存路径过长");
        return false;
    }
    FILE* file = fopen(part, "wb");
    if (!file) {
        publish(BOOK_ORBIT_CATALOG_FAILED, "TF卡无法写入下载缓存");
        return false;
    }
    file_sink_t sink = {.file = file, .written = 0, .total = -1,
                        .failed = false, .last_activity_us = esp_timer_get_time()};
    /* 每次下载重置进度桶；见 download_event 的静态阈值。Reset the bucket each download. */
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CATALOG_DOWNLOAD_TIMEOUT_MS,
        .is_async = true,
        .buffer_size = 4096,
        .buffer_size_tx = 8192,
        .event_handler = download_event,
        .user_data = &sink,
        .crt_bundle_attach = strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        unlink(part);
        publish(BOOK_ORBIT_CATALOG_FAILED, "下载内存不足");
        return false;
    }
    const char* user = app_settings_orbit_user();
    const char* password = app_settings_orbit_password();
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (user) esp_http_client_set_header(client, "x-auth-user", user);
    char userkey[33];
    if (password && book_orbit_userkey(password, userkey)) {
        esp_http_client_set_header(client, "x-auth-key", userkey);
        memset(userkey, 0, sizeof(userkey));
    }
    esp_http_client_set_header(client, "User-Agent", "kiikoread " BOOK_ORBIT_PLUGIN_VERSION);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = download_perform(client, &sink);
    int status = esp_http_client_get_status_code(client);
    long long declared = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);
    fclose(file);
    if (err != ESP_OK || s_cancel || sink.failed || (status != 200 && status != 206)) {
        unlink(part);
        if (s_cancel) {
            publish(BOOK_ORBIT_CATALOG_CANCELLED, NULL);
        } else if (status == 401) {
            publish(BOOK_ORBIT_CATALOG_FAILED, "BookOrbit 账号或密码错误");
        } else if (status == 404) {
            publish(BOOK_ORBIT_CATALOG_FAILED, "服务器上没有这个文件");
        } else if (err == ESP_ERR_TIMEOUT) {
            publish(BOOK_ORBIT_CATALOG_FAILED, "下载超时");
        } else {
            publish(BOOK_ORBIT_CATALOG_FAILED, "下载失败");
        }
        return false;
    }
    if (declared > 0 && sink.written != declared) {
        unlink(part);
        publish(BOOK_ORBIT_CATALOG_FAILED, "下载不完整");
        return false;
    }
    if (rename(part, target) != 0) {
        unlink(part);
        publish(BOOK_ORBIT_CATALOG_FAILED, "TF卡写入失败");
        return false;
    }
    s_status.saved_count++;
    s_status.download_total = 0;
    s_status.download_received = 0;
    ++s_status.revision;
    return true;
}

// ---- 任务执行 / job execution --------------------------------------------------

static void fail_state(const char* message) { publish(BOOK_ORBIT_CATALOG_FAILED, message); }

static void run_job(void) {
    char base[240];
    if (!book_orbit_catalog_api_base(app_settings_orbit_server(), base, sizeof(base))) {
        fail_state("BookOrbit 服务器地址无效");
        return;
    }
    char url[640];
    char* body = NULL;
    int status = 0;
    bool ok = false;
    switch (s_job) {
        case JOB_HOME:
            snprintf(url, sizeof(url), "%s/koreader/plugin/catalog/root", base);
            break;
        case JOB_SECTION:
            snprintf(url, sizeof(url), "%s/koreader/plugin/catalog/sections/%s?page=%d&size=%d",
                     base, s_job_section, s_job_page, BOOK_ORBIT_CATALOG_SECTION_SIZE);
            break;
        case JOB_BOOKS:
            snprintf(url, sizeof(url), "%s/koreader/plugin/catalog/books?page=%d&size=%d",
                     base, s_job_page, BOOK_ORBIT_CATALOG_PAGE_SIZE);
            if (s_job_filter.sort && s_job_filter.sort[0])
                book_orbit_catalog_append_param(url, sizeof(url), "sort", s_job_filter.sort);
            if (s_job_filter.author && s_job_filter.author[0])
                book_orbit_catalog_append_param(url, sizeof(url), "author", s_job_filter.author);
            if (s_job_filter.library_id > 0) {
                char value[24];
                snprintf(value, sizeof(value), "%ld", s_job_filter.library_id);
                book_orbit_catalog_append_param(url, sizeof(url), "libraryId", value);
            }
            if (s_job_filter.collection_id > 0) {
                char value[24];
                snprintf(value, sizeof(value), "%ld", s_job_filter.collection_id);
                book_orbit_catalog_append_param(url, sizeof(url), "collectionId", value);
            }
            if (s_job_filter.smart_scope_id > 0) {
                char value[24];
                snprintf(value, sizeof(value), "%ld", s_job_filter.smart_scope_id);
                book_orbit_catalog_append_param(url, sizeof(url), "smartScopeId", value);
            }
            if (s_job_filter.series_id > 0) {
                char value[24];
                snprintf(value, sizeof(value), "%ld", s_job_filter.series_id);
                book_orbit_catalog_append_param(url, sizeof(url), "seriesId", value);
            }
            break;
        case JOB_DETAIL:
            snprintf(url, sizeof(url), "%s/koreader/plugin/catalog/books/%ld", base, s_job_book_id);
            break;
        case JOB_DOWNLOAD:
            snprintf(url, sizeof(url), "%s/koreader/plugin/catalog/files/%ld/download", base, s_job_file.id);
            if (s_job_file.has_variant && s_job_file.variant[0])
                book_orbit_catalog_append_param(url, sizeof(url), "variant", s_job_file.variant);
            break;
        default:
            return;
    }
    if (s_job != JOB_DOWNLOAD) {
        if (!http_get_json(url, &body, &status)) {
            if (!s_cancel) fail_state(status == 401 ? "BookOrbit 账号或密码错误" : "网络连接失败");
            return;
        }
    }
    switch (s_job) {
        case JOB_HOME:
            ok = parse_root(body);
            break;
        case JOB_SECTION:
            ok = parse_section_list(body);
            break;
        case JOB_BOOKS:
            ok = parse_books(body);
            break;
        case JOB_DETAIL:
            ok = parse_detail(body);
            break;
        case JOB_DOWNLOAD:
            ok = download_file(url, s_job_filename);
            break;
        default:
            ok = false;
            break;
    }
    if (body) heap_caps_free(body);
    if (!ok && !s_cancel && s_job != JOB_DOWNLOAD) fail_state("服务器响应无法解析");
}

static void catalog_worker(void* arg) {
    (void)arg;
    run_job();
    if (s_status.state == BOOK_ORBIT_CATALOG_LOADING) {
        if (s_cancel) {
            s_status.state = BOOK_ORBIT_CATALOG_CANCELLED;
        } else if (s_status.state != BOOK_ORBIT_CATALOG_FAILED) {
            s_status.state = BOOK_ORBIT_CATALOG_DONE;
            s_status.message[0] = 0;
        }
        ++s_status.revision;
    }
    s_running = false;
    vTaskDelete(NULL);
}

// ---- 公开 API / public API -----------------------------------------------------

static esp_err_t start_job(catalog_job_t kind) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!app_settings_orbit_server() || !app_settings_orbit_server()[0] || !app_settings_orbit_user() ||
        !app_settings_orbit_user()[0] || !app_settings_orbit_password() ||
        !app_settings_orbit_password()[0]) {
        publish(BOOK_ORBIT_CATALOG_FAILED, "请先在设置中配置 BookOrbit 账号");
        return ESP_ERR_INVALID_STATE;
    }
    s_job = kind;
    s_cancel = false;
    s_status.state = BOOK_ORBIT_CATALOG_LOADING;
    s_status.busy = true;
    s_status.message[0] = 0;
    s_status.download_total = 0;
    s_status.download_received = 0;
    ++s_status.revision;
    s_running = true;
    int created = xTaskCreate(catalog_worker, "orbit_catalog", CATALOG_STACK_DEPTH, NULL, 5, &s_task);
    if (created != pdPASS) {
        s_running = false;
        s_task = NULL;
        publish(BOOK_ORBIT_CATALOG_FAILED, "后台任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t book_orbit_catalog_fetch_home(void) { return start_job(JOB_HOME); }

esp_err_t book_orbit_catalog_fetch_section(const char* section, int page) {
    if (!section || !section[0]) return ESP_ERR_INVALID_ARG;
    snprintf(s_job_section, sizeof(s_job_section), "%s", section);
    s_job_page = page >= 1 ? page : 1;
    return start_job(JOB_SECTION);
}

esp_err_t book_orbit_catalog_fetch_books(const book_orbit_catalog_books_filter_t* filter, int page) {
    if (filter) {
        s_job_filter = *filter;
        s_job_filter.author = NULL;
        if (filter->author && filter->author[0]) {
            static char author_copy[136]; /* 单请求串行；工作线程独占读取。Serial jobs; worker-only reads. */
            snprintf(author_copy, sizeof(author_copy), "%s", filter->author);
            s_job_filter.author = author_copy;
        }
    } else {
        memset(&s_job_filter, 0, sizeof(s_job_filter));
    }
    s_job_page = page >= 1 ? page : 1;
    return start_job(JOB_BOOKS);
}

esp_err_t book_orbit_catalog_fetch_detail(long book_id) {
    if (book_id <= 0) return ESP_ERR_INVALID_ARG;
    s_job_book_id = book_id;
    return start_job(JOB_DETAIL);
}

esp_err_t book_orbit_catalog_download(const char* file_name, const book_orbit_catalog_file_t* file) {
    if (!file_name || !file_name[0] || !file || file->id <= 0) return ESP_ERR_INVALID_ARG;
    snprintf(s_job_filename, sizeof(s_job_filename), "%s", file_name);
    s_job_file = *file;
    return start_job(JOB_DOWNLOAD);
}

bool book_orbit_catalog_busy(void) { return s_running; }

unsigned book_orbit_catalog_saved_count(void) {
    unsigned count = (unsigned)s_status.saved_count;
    return count;
}

void book_orbit_catalog_get_status(book_orbit_catalog_status_t* out) {
    if (!out) return;
    *out = s_status;
    out->busy = s_running;
}

/// 照 book_orbit_sync 的 join 方式：置位取消后轮询任务退出。
/// Join like book_orbit_sync: set cancel, then poll for the worker to exit.
void book_orbit_catalog_cancel_join(void) {
    if (s_task) {
        s_cancel = true;
        for (int i = 0; i < 500 && s_running; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        s_task = NULL;
        s_running = false;
        s_cancel = false;
    }
}

void book_orbit_catalog_close(void) {
    book_orbit_catalog_cancel_join();
    if (s_detail) {
        heap_caps_free(s_detail);
        s_detail = NULL;
    }
    s_item_count = 0;
    s_book_count = 0;
    s_list_page = 1;
    s_list_total = -1;
    s_list_has_next = false;
    memset(&s_status, 0, sizeof(s_status));
}

int book_orbit_catalog_item_count(void) {
    int count = s_item_count;
    return count;
}

bool book_orbit_catalog_item_get(int index, book_orbit_catalog_item_t* out) {
    if (!out) return false;
    bool ok = index >= 0 && index < s_item_count;
    if (ok) *out = s_items[index];
    return ok;
}

int book_orbit_catalog_book_count(void) {
    int count = s_book_count;
    return count;
}

bool book_orbit_catalog_book_get(int index, book_orbit_catalog_book_t* out) {
    if (!out) return false;
    bool ok = index >= 0 && index < s_book_count;
    if (ok) *out = s_books[index];
    return ok;
}

bool book_orbit_catalog_detail_get(book_orbit_catalog_detail_t* out) {
    if (!out) return false;
    bool ok = s_detail != NULL;
    if (ok) *out = *s_detail;
    return ok;
}

void book_orbit_catalog_list_range(int* page, int* total, bool* has_next) {
    if (page) *page = s_list_page;
    if (total) *total = s_list_total;
    if (has_next) *has_next = s_list_has_next;
}
