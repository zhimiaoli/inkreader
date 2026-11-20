/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 2.0 目录客户端实现。每轮请求起一个一次性工作线程：目录页、搜索页或
 *      acquisition 下载；快照发布后由 UI 通过 revision 轮询。仅解析
 *      application/opds+json（OPDS 2.0），Atom 源直接报"不是 OPDS 2.0 目录"。
 * English: OPDS 2.0 catalog client. Each request spawns a one-shot worker: catalog page,
 *      search page or acquisition download; snapshots are published and the UI polls via
 *      revision. Only application/opds+json (OPDS 2.0) is parsed; Atom sources fail with
 *      a clear "not an OPDS 2.0 catalog" message.
 *
 * 冻结：凭据只读 OPDS 设置（Basic 认证，可匿名）；下载先写 .part 再原子改名；失败
 *      不落盘；不自动下载，全部由界面显式发起。
 * Frozen: credentials read the OPDS settings only (Basic auth, anonymous allowed);
 *      downloads stage through .part and rename atomically; failures never land on the
 *      shelf; nothing downloads without an explicit UI action.
 */

#include "book_opds.h"

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

#include "book_orbit_catalog.h"
#include "book_store.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

#define OPDS_HTTP_TIMEOUT_MS 8000
#define OPDS_DOWNLOAD_TIMEOUT_MS 30000
#define OPDS_RESPONSE_MAX (64U * 1024U)
#define OPDS_FILENAME_MAX 192
#define OPDS_STACK_DEPTH 16384 /* 16KiB：JSON 解析 + TLS 下载 / JSON parsing plus TLS download */

// ---- 快照与状态 / snapshots and status -----------------------------------------

typedef enum {
    JOB_NONE = 0,
    JOB_OPEN,
    JOB_DOWNLOAD,
} opds_job_t;

static TaskHandle_t s_task;
static bool s_running;
static bool s_cancel;
static opds_job_t s_job;
static char s_job_url[BOOK_OPDS_URL_MAX];
static char s_job_filename[OPDS_FILENAME_MAX]; /* 单请求串行；工作线程独占读取。Serial jobs; worker-only reads. */

static book_opds_status_t s_status = {.state = BOOK_OPDS_IDLE};

static book_opds_entry_t s_entries[BOOK_OPDS_ENTRIES_MAX];
static int s_entry_count;
static char s_title[128];
static char s_self[BOOK_OPDS_URL_MAX];
static char s_next[BOOK_OPDS_URL_MAX];
static char s_prev[BOOK_OPDS_URL_MAX];
static char s_search[BOOK_OPDS_URL_MAX];

static void publish(book_opds_state_t state, const char* message) {
    s_status.state = state;
    s_status.busy = state == BOOK_OPDS_LOADING;
    if (state != BOOK_OPDS_LOADING) {
        s_status.download_total = 0;
        s_status.download_received = 0;
    }
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    ++s_status.revision;
}

// ---- 纯函数 / pure helpers -----------------------------------------------------

bool book_opds_resolve_href(const char* base, const char* href, char* out, size_t cap) {
    if (!base || !base[0] || !href || !href[0] || !out || cap < 16) return false;
    if (!strncmp(href, "https://", 8) || !strncmp(href, "http://", 7)) {
        if (strlen(href) + 1 > cap) return false;
        memcpy(out, href, strlen(href) + 1);
    } else {
        /* 相对引用基于 base：scheme://authority[/dir/]。/ Relative refs join
           scheme://authority[/dir/]. */
        const char* after = strstr(base, "://");
        if (!after) return false;
        size_t scheme_len = (size_t)(after - base) + 3;
        const char* authority = base + scheme_len;
        const char* authority_end = authority;
        while (*authority_end && *authority_end != '/' && *authority_end != '?' && *authority_end != '#')
            ++authority_end;
        size_t authority_len = (size_t)(authority_end - authority);
        size_t joined_len;
        if (href[0] == '/' && href[1] == '/') {
            /* //host/...：只补 scheme。/ Scheme-relative: only the scheme is missing. */
            joined_len = strlen(href + 2);
            if (scheme_len + joined_len + 1 > cap) return false;
            memcpy(out, base, scheme_len);
            memcpy(out + scheme_len, href + 2, joined_len + 1);
        } else if (href[0] == '/') {
            joined_len = authority_len + strlen(href);
            if (scheme_len + joined_len + 1 > cap) return false;
            memcpy(out, base, scheme_len + authority_len);
            memcpy(out + scheme_len + authority_len, href, strlen(href) + 1);
        } else if (href[0] == '?' || href[0] == '#') {
            return false; /* 仅 query/fragment 的引用不适合目录导航 / not useful for navigation */
        } else {
            /* 相对路径合并 base 的目录段（剥 query/fragment 与最后一段路径）。 */
            /* Relative paths join the base's directory segment (drop query/# and the
               last path segment). */
            const char* cut = NULL;
            for (const char* p = authority_end; *p && *p != '?' && *p != '#'; ++p)
                if (*p == '/') cut = p;
            if (!cut) return false; /* base 无路径段，无法合并 / nothing to join against */
            authority_len = (size_t)(cut - base) - scheme_len + 1;
            joined_len = authority_len + strlen(href);
            if (scheme_len + joined_len + 1 > cap) return false;
            memcpy(out, base, scheme_len + authority_len);
            memcpy(out + scheme_len + authority_len, href, strlen(href) + 1);
        }
    }
    /* 去掉 fragment。/ Drop the fragment. */
    char* frag = strchr(out, '#');
    if (frag) *frag = 0;
    return out[0] != 0;
}

static bool uri_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static size_t append_encoded(char* out, size_t cap, size_t length, const char* text) {
    static const char HEX[] = "0123456789ABCDEF";
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if (uri_unreserved(*p)) {
            if (length + 1 >= cap) return 0;
            out[length++] = (char)*p;
        } else {
            if (length + 3 >= cap) return 0;
            out[length++] = '%';
            out[length++] = HEX[*p >> 4];
            out[length++] = HEX[*p & 15];
        }
    }
    return length;
}

bool book_opds_expand_search(const char* templated, const char* query, char* out, size_t cap) {
    if (!out || !cap || !query || !query[0]) return false;
    const char* tpl = templated && templated[0] ? templated : NULL;
    if (tpl && !strstr(tpl, "{query}") && !strstr(tpl, "{searchTerms}") && !strchr(tpl, '{')) {
        tpl = NULL; /* 纯 URL 无模板：按 ?query= 追加 / plain URL: append ?query= */
    }
    if (!tpl) {
        size_t length = 0;
        if (templated && templated[0]) {
            length = strlen(templated);
            if (length + 1 > cap) return false;
            memcpy(out, templated, length + 1);
        } else {
            out[0] = 0;
        }
        if (length + 2 >= cap) return false;
        out[length++] = strchr(out, '?') ? '&' : '?';
        if (length + 7 > cap) return false;
        memcpy(out + length, "query=", 7);
        length += 6;
        length = append_encoded(out, cap, length, query);
        if (!length) return false;
        out[length] = 0;
        return true;
    }
    char encoded[256];
    size_t encoded_len = append_encoded(encoded, sizeof(encoded), 0, query);
    if (!encoded_len) return false;
    encoded[encoded_len] = 0;
    /* 依序处理 {?var=default} / {?var} / {query} / {searchTerms}。 */
    /* Process {?var=default} / {?var} / {query} / {searchTerms}, left to right. */
    size_t length = 0;
    for (const char* p = tpl; *p;) {
        if (*p == '{') {
            const char* close = strchr(p, '}');
            if (!close || close - p > 64) return false;
            char expr[68];
            size_t expr_len = (size_t)(close - p - 1);
            memcpy(expr, p + 1, expr_len);
            expr[expr_len] = 0;
            if (expr[0] == '?' || expr[0] == '&') {
                char* eq = strchr(expr, '=');
                if (eq) *eq = 0;
                const char* var = expr + 1;
                if (length + 1 + strlen(var) + 1 + encoded_len + 1 > cap) return false;
                bool first_query = expr[0] == '?' && !memchr(out, '?', length);
                out[length++] = first_query ? '?' : '&';
                memcpy(out + length, var, strlen(var));
                length += strlen(var);
                out[length++] = '=';
                memcpy(out + length, encoded, encoded_len + 1);
                length += encoded_len;
            } else if (!strcmp(expr, "query") || !strcmp(expr, "searchTerms")) {
                if (length + encoded_len + 1 > cap) return false;
                memcpy(out + length, encoded, encoded_len + 1);
                length += encoded_len;
            } else {
                return false; /* 未知模板表达式 / unknown template expression */
            }
            p = close + 1;
            continue;
        }
        if (length + 1 >= cap) return false;
        out[length++] = *p++;
    }
    out[length] = 0;
    return length > 0;
}

bool book_opds_format_name(const char* media_type, char* out, size_t cap) {
    if (!out || cap < 5) return false;
    if (!media_type || !media_type[0]) {
        snprintf(out, cap, "BOOK");
        return true;
    }
    if (!strcasecmp(media_type, "application/epub+zip")) { snprintf(out, cap, "EPUB"); return true; }
    if (!strcasecmp(media_type, "application/pdf")) { snprintf(out, cap, "PDF"); return true; }
    if (!strcasecmp(media_type, "application/x-mobipocket-ebook")) { snprintf(out, cap, "MOBI"); return true; }
    if (!strcasecmp(media_type, "text/plain")) { snprintf(out, cap, "TXT"); return true; }
    if (!strcasecmp(media_type, "application/fb2+zip") ||
        !strcasecmp(media_type, "application/x-fictionbook+xml")) {
        snprintf(out, cap, "FB2");
        return true;
    }
    if (!strcasecmp(media_type, "application/zip")) { snprintf(out, cap, "ZIP"); return true; }
    /* subtype 最后一段大写。/ Uppercased last token of the subtype. */
    const char* slash = strchr(media_type, '/');
    const char* sub = slash ? slash + 1 : media_type;
    const char* plus = strchr(sub, '+');
    size_t want = plus ? (size_t)(plus - sub) : strlen(sub);
    if (!want) { snprintf(out, cap, "BOOK"); return true; }
    if (want >= cap) want = cap - 1;
    for (size_t i = 0; i < want; ++i) out[i] = (char)(sub[i] >= 'a' && sub[i] <= 'z' ? sub[i] - 32 : sub[i]);
    out[want] = 0;
    return true;
}

/// 标准 base64（带 padding）。/ Standard base64 (padded).
bool book_opds_basic_auth(const char* user, const char* password, char* out, size_t cap) {
    if (!out || cap < 8) return false;
    char joined[224];
    int n = snprintf(joined, sizeof(joined), "%s:%s", user ? user : "", password ? password : "");
    if (n < 0 || (size_t)n >= sizeof(joined)) return false;
    static const char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in_len = (size_t)n;
    size_t needed = (in_len * 4 + 2) / 3 + 1;
    if (needed > cap) return false;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        unsigned v = (unsigned)(unsigned char)joined[i] << 16;
        int pad = 0;
        if (i + 1 < in_len) v |= (unsigned)(unsigned char)joined[i + 1] << 8;
        else ++pad;
        if (i + 2 < in_len) v |= (unsigned)(unsigned char)joined[i + 2];
        else ++pad;
        out[o++] = TABLE[(v >> 18) & 63];
        out[o++] = TABLE[(v >> 12) & 63];
        out[o++] = pad >= 2 ? '=' : TABLE[(v >> 6) & 63];
        out[o++] = pad >= 1 ? '=' : TABLE[v & 63];
    }
    out[o] = 0;
    return true;
}

/// 从 media type 推导下载扩展名。/ Download extension from the media type.
static bool extension_of(const char* media_type, char* out, size_t cap) {
    if (!strcasecmp(media_type, "application/epub+zip")) return snprintf(out, cap, "epub") > 0;
    if (!strcasecmp(media_type, "application/pdf")) return snprintf(out, cap, "pdf") > 0;
    if (!strcasecmp(media_type, "application/x-mobipocket-ebook")) return snprintf(out, cap, "mobi") > 0;
    if (!strcasecmp(media_type, "text/plain")) return snprintf(out, cap, "txt") > 0;
    if (!strcasecmp(media_type, "application/fb2+zip")) return snprintf(out, cap, "fb2.zip") > 0;
    if (!strcasecmp(media_type, "application/zip")) return snprintf(out, cap, "zip") > 0;
    const char* slash = strchr(media_type, '/');
    const char* sub = slash ? slash + 1 : media_type;
    const char* plus = strchr(sub, '+');
    size_t want = plus ? (size_t)(plus - sub) : strlen(sub);
    if (!want) return snprintf(out, cap, "epub") > 0;
    if (want >= cap) want = cap - 1;
    for (size_t i = 0; i < want; ++i) out[i] = (char)(sub[i] >= 'A' && sub[i] <= 'Z' ? sub[i] + 32 : sub[i]);
    out[want] = 0;
    return true;
}

/// acquisition 链接偏好：EPUB > PDF > TXT > 其他 > ZIP。/ Acquisition preference:
/// EPUB > PDF > TXT > others > ZIP.
static int acquisition_score(const char* media_type) {
    if (!strcasecmp(media_type, "application/epub+zip")) return 100;
    if (!strcasecmp(media_type, "application/pdf")) return 80;
    if (!strcasecmp(media_type, "text/plain")) return 70;
    if (!strcasecmp(media_type, "application/x-mobipocket-ebook")) return 60;
    if (!strcasecmp(media_type, "application/fb2+zip")) return 50;
    if (!strcasecmp(media_type, "application/zip")) return 10;
    return 40;
}

// ---- JSON 解析 / JSON parsing --------------------------------------------------

static const char* json_string(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
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

/// 是否 acquisition 链接：规范 rel 或宽容包含 "acquisition"。/ Acquisition check: the
/// spec rel or a lenient substring.
static bool is_acquisition_rel(const char* rel) {
    if (!rel) return false;
    if (!strcasecmp(rel, "http://opds-spec.org/acquisition")) return true;
    return strstr(rel, "acquisition") != NULL;
}

/// 一个 publication → 快照条目（含首选下载链接与格式）。/ Publication → snapshot entry
/// with its preferred acquisition link and format.
static void parse_publication(const cJSON* pub, const char* base_url, book_opds_entry_t* out) {
    memset(out, 0, sizeof(*out));
    out->publication = true;
    const cJSON* meta = cJSON_GetObjectItem(pub, "metadata");
    copy_utf8_bounded(out->title, sizeof(out->title),
                      cJSON_IsObject(meta) ? json_string(meta, "title") : json_string(pub, "title"),
                      150);
    const cJSON* authors = cJSON_IsObject(meta) ? cJSON_GetObjectItem(meta, "author") : NULL;
    if (!cJSON_IsArray(authors)) authors = cJSON_GetObjectItem(pub, "author");
    if (cJSON_IsArray(authors)) {
        size_t used = 0;
        int count = cJSON_GetArraySize(authors);
        for (int a = 0; a < count && a < 2; ++a) {
            const cJSON* author = cJSON_GetArrayItem(authors, a);
            const char* name = cJSON_IsObject(author) ? json_string(author, "name")
                               : cJSON_IsString(author) ? author->valuestring : NULL;
            if (!name || !name[0]) continue;
            size_t want = strlen(name);
            if (used && used + 2 < sizeof(out->authors)) {
                memcpy(out->authors + used, ", ", 2);
                used += 2;
            }
            if (used + want >= sizeof(out->authors)) want = sizeof(out->authors) - used - 1;
            if (want) {
                memcpy(out->authors + used, name, want);
                used += want;
                out->authors[used] = 0;
            }
        }
    }
    /* 在 links 里挑首选 acquisition 直链。/ Pick the preferred acquisition link. */
    const cJSON* links = cJSON_GetObjectItem(pub, "links");
    if (!cJSON_IsArray(links)) return;
    int count = cJSON_GetArraySize(links);
    int best_score = -1;
    const char* best_href = NULL;
    char best_type[48] = "";
    for (int i = 0; i < count; ++i) {
        const cJSON* link = cJSON_GetArrayItem(links, i);
        if (!cJSON_IsObject(link)) continue;
        const char* rel = json_string(link, "rel");
        const char* href = json_string(link, "href");
        if (!is_acquisition_rel(rel) || !href || !href[0]) continue;
        const char* type = json_string(link, "type");
        char media[48];
        copy_utf8_bounded(media, sizeof(media), type, 44);
        int score = acquisition_score(media);
        if (score > best_score) {
            best_score = score;
            best_href = href;
            snprintf(best_type, sizeof(best_type), "%s", media);
        }
    }
    if (!best_href) return;
    if (!book_opds_resolve_href(base_url, best_href, out->href, sizeof(out->href))) return;
    snprintf(out->media_type, sizeof(out->media_type), "%s", best_type);
    book_opds_format_name(out->media_type, out->format, sizeof(out->format));
}

/// 解析一份 OPDS 2.0 目录 JSON。/ Parse one OPDS 2.0 catalog JSON.
bool book_opds_parse_catalog(const char* json, const char* request_url, char* out_title,
                             size_t title_cap, book_opds_entry_t* out_entries, int out_max,
                             int* out_count, char* out_next, char* out_prev, char* out_search,
                             char* out_self, size_t url_cap) {
    if (!json || !request_url || !request_url[0] || !out_title || !out_entries || !out_count ||
        !out_next || !out_prev || !out_search || !out_self || url_cap < 16)
        return false;
    /* 相对链接基准：self 尚未出现时用请求 URL。/ Relative base: the request URL until
       self is seen. */
    const char* link_base = request_url;
#define OPDS_LINK_BASE() (out_self[0] ? out_self : link_base)
    cJSON* root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    /* Atom（OPDS 1.x）会被 cJSON 解析成含 "<feed>" 键的对象；键存在即拒绝。 */
    /* Atom (OPDS 1.x) parses into an object with a "<feed>" key; its presence rejects. */
    if (cJSON_GetObjectItem(root, "<feed") || cJSON_GetObjectItem(root, "feed")) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON* meta = cJSON_GetObjectItem(root, "metadata");
    copy_utf8_bounded(out_title, title_cap,
                      cJSON_IsObject(meta) ? json_string(meta, "title") : json_string(root, "title"),
                      120);
    out_next[0] = 0;
    out_prev[0] = 0;
    out_search[0] = 0;
    out_self[0] = 0;
    const cJSON* links = cJSON_GetObjectItem(root, "links");
    if (cJSON_IsArray(links)) {
        int count = cJSON_GetArraySize(links);
        for (int i = 0; i < count; ++i) {
            const cJSON* link = cJSON_GetArrayItem(links, i);
            if (!cJSON_IsObject(link)) continue;
            const char* rel = json_string(link, "rel");
            const char* href = json_string(link, "href");
            if (!rel || !href || !href[0]) continue;
            /* self 尚未确定时以请求 URL 为基准；self 确定后以其为基准。 */
            /* Resolve against the request URL until self is known, then against self. */
            char resolved[BOOK_OPDS_URL_MAX];
            if (!book_opds_resolve_href(OPDS_LINK_BASE(), href, resolved, sizeof(resolved)))
                continue;
            if (!strcmp(rel, "self")) snprintf(out_self, url_cap, "%s", resolved);
            else if (!strcmp(rel, "next")) snprintf(out_next, url_cap, "%s", resolved);
            else if (!strcmp(rel, "prev")) snprintf(out_prev, url_cap, "%s", resolved);
            else if (!strcmp(rel, "search")) snprintf(out_search, url_cap, "%s", resolved);
        }
    }
    int kept = 0;
    /* navigation：{href,title}。/ navigation: {href,title}. */
    const cJSON* navigation = cJSON_GetObjectItem(root, "navigation");
    if (cJSON_IsArray(navigation)) {
        int count = cJSON_GetArraySize(navigation);
        for (int i = 0; i < count && kept < out_max; ++i) {
            const cJSON* entry = cJSON_GetArrayItem(navigation, i);
            if (!cJSON_IsObject(entry)) continue;
            const char* href = json_string(entry, "href");
            if (!href || !href[0]) continue;
            book_opds_entry_t* item = &out_entries[kept];
            memset(item, 0, sizeof(*item));
            if (!book_opds_resolve_href(OPDS_LINK_BASE(), href, item->href, sizeof(item->href)))
                continue;
            copy_utf8_bounded(item->title, sizeof(item->title), json_string(entry, "title"), 150);
            ++kept;
        }
    }
    /* publications：{metadata, links}。/ publications: {metadata,links}. */
    const cJSON* publications = cJSON_GetObjectItem(root, "publications");
    if (cJSON_IsArray(publications)) {
        int count = cJSON_GetArraySize(publications);
        for (int i = 0; i < count && kept < out_max; ++i) {
            const cJSON* pub = cJSON_GetArrayItem(publications, i);
            if (!cJSON_IsObject(pub)) continue;
            parse_publication(pub, OPDS_LINK_BASE(), &out_entries[kept]);
            if (!out_entries[kept].href[0]) continue;
            ++kept;
        }
    }
    /* groups：扁平化为导航条目（组内 self/start 或首个 navigation href）。 */
    /* groups: flatten into navigation entries (group self/start or first nav href). */
    const cJSON* groups = cJSON_GetObjectItem(root, "groups");
    if (cJSON_IsArray(groups)) {
        int count = cJSON_GetArraySize(groups);
        for (int i = 0; i < count && kept < out_max; ++i) {
            const cJSON* group = cJSON_GetArrayItem(groups, i);
            if (!cJSON_IsObject(group)) continue;
            const char* href = NULL;
            const cJSON* group_links = cJSON_GetObjectItem(group, "links");
            if (cJSON_IsArray(group_links)) {
                int link_count = cJSON_GetArraySize(group_links);
                for (int l = 0; l < link_count && !href; ++l) {
                    const cJSON* link = cJSON_GetArrayItem(group_links, l);
                    if (!cJSON_IsObject(link)) continue;
                    const char* rel = json_string(link, "rel");
                    if (rel && (!strcmp(rel, "self") || !strcmp(rel, "start")))
                        href = json_string(link, "href");
                }
            }
            if (!href) {
                const cJSON* group_nav = cJSON_GetObjectItem(group, "navigation");
                if (cJSON_IsArray(group_nav)) {
                    const cJSON* first = cJSON_GetArrayItem(group_nav, 0);
                    href = cJSON_IsObject(first) ? json_string(first, "href") : NULL;
                }
            }
            if (!href || !href[0]) continue;
            book_opds_entry_t* item = &out_entries[kept];
            memset(item, 0, sizeof(*item));
            if (!book_opds_resolve_href(OPDS_LINK_BASE(), href, item->href, sizeof(item->href)))
                continue;
            const cJSON* group_meta = cJSON_GetObjectItem(group, "metadata");
            copy_utf8_bounded(item->title, sizeof(item->title),
                              cJSON_IsObject(group_meta) ? json_string(group_meta, "title")
                                                         : json_string(group, "title"),
                              150);
            ++kept;
        }
    }
    *out_count = kept;
    cJSON_Delete(root);
    return true;
#undef OPDS_LINK_BASE
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

/// GET JSON（Basic 认证）：成功时 *out 为 PSRAM 缓冲（调用方 free）。
/// GET JSON (Basic auth): on success *out is a PSRAM buffer (caller frees).
static bool http_get_json(const char* url, char** out, int* status) {
    *out = NULL;
    *status = 0;
    if (s_cancel) return false;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return false;
    char* buf = heap_caps_malloc(OPDS_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;
    buf[0] = 0;
    json_sink_t sink = {.buf = buf, .cap = OPDS_RESPONSE_MAX, .length = 0, .overflow = false};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OPDS_HTTP_TIMEOUT_MS,
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
    esp_http_client_set_header(client, "Accept", "application/opds+json, application/json");
    esp_http_client_set_header(client, "User-Agent", "kiikoread-opds-1");
    const char* user = app_settings_opds_user();
    const char* password = app_settings_opds_password();
    char auth[232];
    if (user && user[0] && book_opds_basic_auth(user, password, auth, sizeof(auth))) {
        esp_http_client_set_header(client, "Authorization", auth);
        memset(auth, 0, sizeof(auth));
    }
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
        if (esp_timer_get_time() - sink->last_activity_us >= (int64_t)OPDS_DOWNLOAD_TIMEOUT_MS * 1000)
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
        publish(BOOK_OPDS_FAILED, "没有可用的存储");
        return false;
    }
    char target[BOOK_STORE_PATH_MAX + OPDS_FILENAME_MAX];
    if (!pick_target(root.path, file_name, target, sizeof(target))) {
        publish(BOOK_OPDS_FAILED, "文件名过长");
        return false;
    }
    char part[BOOK_STORE_PATH_MAX + OPDS_FILENAME_MAX];
    if (snprintf(part, sizeof(part), "%s/.opds.part", root.path) >= (int)sizeof(part)) {
        publish(BOOK_OPDS_FAILED, "下载缓存路径过长");
        return false;
    }
    FILE* file = fopen(part, "wb");
    if (!file) {
        publish(BOOK_OPDS_FAILED, "TF卡无法写入下载缓存");
        return false;
    }
    file_sink_t sink = {.file = file, .written = 0, .total = -1,
                        .failed = false, .last_activity_us = esp_timer_get_time()};
    /* 每次下载重置进度桶；见 download_event 的静态阈值。Reset the bucket each download. */
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OPDS_DOWNLOAD_TIMEOUT_MS,
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
        publish(BOOK_OPDS_FAILED, "下载内存不足");
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "kiikoread-opds-1");
    const char* user = app_settings_opds_user();
    const char* password = app_settings_opds_password();
    char auth[232];
    if (user && user[0] && book_opds_basic_auth(user, password, auth, sizeof(auth))) {
        esp_http_client_set_header(client, "Authorization", auth);
        memset(auth, 0, sizeof(auth));
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = download_perform(client, &sink);
    int status = esp_http_client_get_status_code(client);
    long long declared = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);
    fclose(file);
    if (err != ESP_OK || s_cancel || sink.failed || (status != 200 && status != 206)) {
        unlink(part);
        if (s_cancel) {
            publish(BOOK_OPDS_CANCELLED, NULL);
        } else if (status == 401) {
            publish(BOOK_OPDS_FAILED, "OPDS 账号或密码错误");
        } else if (status == 404) {
            publish(BOOK_OPDS_FAILED, "服务器上没有这个文件");
        } else if (err == ESP_ERR_TIMEOUT) {
            publish(BOOK_OPDS_FAILED, "下载超时");
        } else {
            publish(BOOK_OPDS_FAILED, "下载失败");
        }
        return false;
    }
    if (declared > 0 && sink.written != declared) {
        unlink(part);
        publish(BOOK_OPDS_FAILED, "下载不完整");
        return false;
    }
    if (rename(part, target) != 0) {
        unlink(part);
        publish(BOOK_OPDS_FAILED, "TF卡写入失败");
        return false;
    }
    s_status.saved_count++;
    s_status.download_total = 0;
    s_status.download_received = 0;
    ++s_status.revision;
    return true;
}

// ---- 任务执行 / job execution --------------------------------------------------

static void fail_state(const char* message) { publish(BOOK_OPDS_FAILED, message); }

static void run_job(void) {
    if (s_job == JOB_DOWNLOAD) {
        download_file(s_job_url, s_job_filename);
        return;
    }
    char* body = NULL;
    int status = 0;
    if (!http_get_json(s_job_url, &body, &status)) {
        if (!s_cancel)
            fail_state(status == 401 ? "OPDS 账号或密码错误"
                                     : status == 404 ? "目录页不存在"
                                                     : "网络连接失败");
        return;
    }
    char title[128];
    char next[BOOK_OPDS_URL_MAX];
    char prev[BOOK_OPDS_URL_MAX];
    char search[BOOK_OPDS_URL_MAX];
    char self[BOOK_OPDS_URL_MAX];
    int count = 0;
    bool     ok = book_opds_parse_catalog(body, s_job_url, title, sizeof(title), s_entries,
                                      BOOK_OPDS_ENTRIES_MAX, &count, next, prev, search, self,
                                      sizeof(self));
    heap_caps_free(body);
    if (!ok) {
        if (!s_cancel) fail_state("不是 OPDS 2.0 目录（JSON）");
        return;
    }
    if (!count) {
        if (!s_cancel) fail_state("目录页没有可浏览条目");
        return;
    }
    s_entry_count = count;
    snprintf(s_title, sizeof(s_title), "%s", title);
    snprintf(s_self, sizeof(s_self), "%s", self[0] ? self : s_job_url);
    snprintf(s_next, sizeof(s_next), "%s", next);
    snprintf(s_prev, sizeof(s_prev), "%s", prev);
    snprintf(s_search, sizeof(s_search), "%s", search);
    ++s_status.revision;
}

static void opds_worker(void* arg) {
    (void)arg;
    run_job();
    if (s_status.state == BOOK_OPDS_LOADING) {
        if (s_cancel) {
            s_status.state = BOOK_OPDS_CANCELLED;
        } else if (s_status.state != BOOK_OPDS_FAILED) {
            s_status.state = BOOK_OPDS_DONE;
            s_status.message[0] = 0;
        }
        ++s_status.revision;
    }
    s_running = false;
    vTaskDelete(NULL);
}

// ---- 公开 API / public API -----------------------------------------------------

static esp_err_t start_job(opds_job_t kind, const char* url) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (kind == JOB_OPEN && (!app_settings_opds_server() || !app_settings_opds_server()[0])) {
        publish(BOOK_OPDS_FAILED, "请先在设置中配置 OPDS 服务器");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_job_url, sizeof(s_job_url), "%s", url);
    s_job = kind;
    s_cancel = false;
    s_status.state = BOOK_OPDS_LOADING;
    s_status.busy = true;
    s_status.message[0] = 0;
    s_status.download_total = 0;
    s_status.download_received = 0;
    ++s_status.revision;
    s_running = true;
    int created = xTaskCreate(opds_worker, "opds_catalog", OPDS_STACK_DEPTH, NULL, 5, &s_task);
    if (created != pdPASS) {
        s_running = false;
        s_task = NULL;
        publish(BOOK_OPDS_FAILED, "后台任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t book_opds_open(const char* url) { return start_job(JOB_OPEN, url); }

esp_err_t book_opds_open_home(void) { return start_job(JOB_OPEN, app_settings_opds_server()); }

esp_err_t book_opds_search(const char* query) {
    if (!query || !query[0]) return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!s_search[0]) {
        publish(BOOK_OPDS_FAILED, "这个目录不支持搜索");
        return ESP_ERR_INVALID_STATE;
    }
    char url[BOOK_OPDS_URL_MAX];
    if (!book_opds_expand_search(s_search, query, url, sizeof(url))) {
        publish(BOOK_OPDS_FAILED, "搜索地址无法展开");
        return ESP_ERR_INVALID_STATE;
    }
    return start_job(JOB_OPEN, url);
}

esp_err_t book_opds_next_page(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!s_next[0]) {
        publish(BOOK_OPDS_FAILED, "没有下一页");
        return ESP_ERR_INVALID_STATE;
    }
    return start_job(JOB_OPEN, s_next);
}

esp_err_t book_opds_prev_page(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!s_prev[0]) {
        publish(BOOK_OPDS_FAILED, "没有上一页");
        return ESP_ERR_INVALID_STATE;
    }
    return start_job(JOB_OPEN, s_prev);
}

esp_err_t book_opds_download(int entry_index) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (entry_index < 0 || entry_index >= s_entry_count) return ESP_ERR_INVALID_ARG;
    const book_opds_entry_t* entry = &s_entries[entry_index];
    if (!entry->publication || !entry->href[0]) return ESP_ERR_INVALID_ARG;
    char ext[16];
    extension_of(entry->media_type, ext, sizeof(ext));
    char filename[OPDS_FILENAME_MAX];
    if (!book_orbit_catalog_build_filename(entry->title[0] ? entry->title : "book", ext, filename,
                                           sizeof(filename))) {
        publish(BOOK_OPDS_FAILED, "文件名构造失败");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_job_filename, sizeof(s_job_filename), "%s", filename);
    return start_job(JOB_DOWNLOAD, entry->href);
}

bool book_opds_busy(void) { return s_running; }

unsigned book_opds_saved_count(void) { return (unsigned)s_status.saved_count; }

void book_opds_get_status(book_opds_status_t* out) {
    if (!out) return;
    *out = s_status;
    out->busy = s_running;
}

/// 照 book_orbit_catalog 的 join 方式：置位取消后轮询任务退出。
/// Join like book_orbit_catalog: set cancel, then poll for the worker to exit.
void book_opds_cancel_join(void) {
    if (s_task) {
        s_cancel = true;
        for (int i = 0; i < 500 && s_running; ++i) vTaskDelay(pdMS_TO_TICKS(20));
        s_task = NULL;
        s_running = false;
        s_cancel = false;
    }
}

void book_opds_close(void) {
    book_opds_cancel_join();
    s_entry_count = 0;
    s_title[0] = 0;
    s_self[0] = 0;
    s_next[0] = 0;
    s_prev[0] = 0;
    s_search[0] = 0;
    memset(&s_status, 0, sizeof(s_status));
}

bool book_opds_title_get(char* out, size_t cap) {
    if (!out || !cap) return false;
    snprintf(out, cap, "%s", s_title);
    return s_title[0] != 0;
}

int book_opds_entry_count(void) { return s_entry_count; }

bool book_opds_entry_get(int index, book_opds_entry_t* out) {
    if (!out) return false;
    bool ok = index >= 0 && index < s_entry_count;
    if (ok) *out = s_entries[index];
    return ok;
}

void book_opds_links_get(bool* has_next, bool* has_prev, bool* has_search) {
    if (has_next) *has_next = s_next[0] != 0;
    if (has_prev) *has_prev = s_prev[0] != 0;
    if (has_search) *has_search = s_search[0] != 0;
}

bool book_opds_self_get(char* out, size_t cap) {
    if (!out || !cap) return false;
    snprintf(out, cap, "%s", s_self[0] ? s_self : s_job_url);
    return out[0] != 0;
}
