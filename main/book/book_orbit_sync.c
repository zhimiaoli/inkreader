/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BookOrbit 同步实现。协议端点与请求/响应格式对齐 agosez/CrossInk-Bookorbit 的
 *      lib/BookOrbitSync（BookOrbit 的 KOReader 兼容 API，挂在 {server}/api/v1/koreader）：
 *      GET  /users/auth                       验证账号（x-auth-user / x-auth-key=MD5(密码)）
 *      GET  /syncs/progress/:document         取进度
 *      PUT  /syncs/progress                   传进度（额外接受 timestamp 破同百分比平局）
 *      POST /plugin/page-stats                逐页阅读事件（服务器聚成阅读 session）
 *      POST /plugin/annotations/exchange      高亮交换（本轮 keysComplete=false 只拉不删）
 *      POST /plugin/annotations/exchange-ack  确认已落地的高亮
 *      GET  /plugin/version                   能力表（仅探测，失败静默跳过）
 *      POST /plugin/sweeps                    记录一次同步，抑制服务器估算 session
 * English: BookOrbit sync implementation. Endpoints and payload shapes follow
 *      agosez/CrossInk-Bookorbit's lib/BookOrbitSync (BookOrbit's KOReader-compatible API
 *      mounted at {server}/api/v1/koreader); see the Chinese list above for the routes.
 *
 * 冻结：不改本地进度 NVS 写路径；远端进度只写待用建议；高亮只拉取展示；设备暂不产生高亮。
 * Frozen: local progress NVS writes stay with the reader; remote progress only lands as a
 *      resume suggestion; highlights are pull-only; the device never mints highlights yet.
 */
#include "book_orbit_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"

#include "book_progress.h"
#include "book_title.h"
#include "settings.h"

static const char *TAG = "orbit";

#define BOOK_ORBIT_BASE_SUFFIX "/api/v1/koreader"
#define BOOK_ORBIT_COOLDOWN_S 600
#define BOOK_ORBIT_BATTERY_MIN 10
#define BOOK_ORBIT_RESPONSE_MAX (48 * 1024)
#define BOOK_ORBIT_CLOCK_MIN 1704067200u /* 2024-01-01 UTC */
#define BOOK_ORBIT_EVENT_RAM_MAX 32
#define BOOK_ORBIT_URL_MAX 224
/// 端点 URL 缓冲：归一化后的基址 + 最长端点（含 :document 哈希）都放得下，避免截断告警。
/// Endpoint URL buffer: normalized base plus the longest route (including :document) fits.
#define BOOK_ORBIT_URL_BUF (BOOK_ORBIT_URL_MAX + 96)
#define BOOK_ORBIT_IDLE_TICKS 120 /* 500ms × 120 = 60s 空闲退出 */

// ---- MD5（小型可移植实现，文档哈希与宿主测试共用） -------------------------------
typedef struct {
    uint32_t a, b, c, d;
    uint32_t lo, hi;
    uint8_t buffer[64];
    size_t used;
} orbit_md5_t;

static uint32_t md5_rot(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

static void md5_block(orbit_md5_t *m, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const int R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 |
               (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rot(a + f + K[i] + x[g], R[i]);
        a = tmp;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5_init(orbit_md5_t *m) {
    m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
    m->lo = m->hi = 0;
    m->used = 0;
}

static void md5_update(orbit_md5_t *m, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    m->lo += (uint32_t)len;
    if (m->lo < (uint32_t)len) ++m->hi;
    while (len) {
        size_t room = 64 - m->used;
        size_t take = len < room ? len : room;
        memcpy(m->buffer + m->used, p, take);
        m->used += take;
        p += take;
        len -= take;
        if (m->used == 64) { md5_block(m, m->buffer); m->used = 0; }
    }
}

static void md5_final(orbit_md5_t *m, uint8_t out[16]) {
    /* 位长在填充前锁定，否则填充字节会计入长度。 */
    /* Latch the bit count before padding or the padding bytes would be counted. */
    uint64_t bits = ((uint64_t)m->hi << 32 | m->lo) * 8;
    m->buffer[m->used++] = 0x80;
    if (m->used > 56) {
        memset(m->buffer + m->used, 0, 64 - m->used);
        md5_block(m, m->buffer);
        m->used = 0;
    }
    memset(m->buffer + m->used, 0, 56 - m->used);
    for (int i = 0; i < 8; ++i) m->buffer[56 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(m, m->buffer);
    m->used = 0;
    uint32_t regs[4] = {m->a, m->b, m->c, m->d};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(regs[i] >> (8 * j));
}

static void md5_hex(const uint8_t digest[16], char out[33]) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out[i * 2] = HEX[digest[i] >> 4];
        out[i * 2 + 1] = HEX[digest[i] & 15];
    }
    out[32] = 0;
}

static void md5_text_hex(const char *text, size_t len, char out[33]) {
    orbit_md5_t m;
    md5_init(&m);
    md5_update(&m, text, len);
    uint8_t digest[16];
    md5_final(&m, digest);
    md5_hex(digest, out);
}

static void orbit_device_id(char out[32]) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(out, 32, "kiikoread-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- 纯函数 -------------------------------------------------------------------
bool book_orbit_format_datetime(uint32_t epoch_seconds, char out[20]) {
    time_t t = (time_t)epoch_seconds;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return false;
    int n = snprintf(out, 20, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                     tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return n == 19;
}

bool book_orbit_parse_datetime(const char *datetime, uint32_t *out_epoch_seconds) {
    if (!datetime || strlen(datetime) != 19 || datetime[4] != '-' || datetime[7] != '-' ||
        datetime[10] != ' ' || datetime[13] != ':' || datetime[16] != ':')
        return false;
    int y, mo, d, h, mi, s;
    if (sscanf(datetime, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return false;
    if (y < 1970 || y > 2999 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || s > 59)
        return false;
    static const unsigned char DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned limit = DAYS[mo - 1];
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) limit = 29;
    if ((unsigned)d > limit) return false;
    /* days_from_civil（Howard Hinnant 算法），与参考实现一致 */
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    int64_t epoch = days * 86400 + h * 3600 + mi * 60 + s;
    if (epoch < 0 || epoch > (int64_t)UINT32_MAX) return false;
    *out_epoch_seconds = (uint32_t)epoch;
    return true;
}

bool book_orbit_annotation_key(const char *datetime, const char *pos0, char out[33]) {
    if (!datetime || !pos0 || !datetime[0] || !pos0[0]) return false;
    orbit_md5_t m;
    md5_init(&m);
    md5_update(&m, datetime, strlen(datetime));
    md5_update(&m, pos0, strlen(pos0));
    uint8_t digest[16];
    md5_final(&m, digest);
    md5_hex(digest, out);
    return true;
}

bool book_orbit_normalize_server_url(const char *in, char *out, size_t cap) {
    if (!in || !out || !cap) return false;
    char trimmed[BOOK_ORBIT_URL_MAX];
    size_t n = 0;
    while (*in == ' ' || *in == '\t') ++in;
    for (; *in && n + 1 < sizeof(trimmed); ++in) {
        if (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') break;
        trimmed[n++] = *in;
    }
    trimmed[n] = 0;
    while (n > 0 && trimmed[n - 1] == '/') trimmed[--n] = 0;
    /* 粘贴完整 API 地址时剥离后缀 */
    static const char *SUFFIXES[] = {"/api/v1/koreader", "/api/v1"};
    for (unsigned i = 0; i < 2; ++i) {
        size_t len = strlen(SUFFIXES[i]);
        if (n >= len && !strcasecmp(trimmed + n - len, SUFFIXES[i])) {
            n -= len;
            trimmed[n] = 0;
            while (n > 0 && trimmed[n - 1] == '/') trimmed[--n] = 0;
        }
    }
    if (!n) return false;
    const char *body = trimmed;
    char with_scheme[BOOK_ORBIT_URL_MAX];
    if (strncasecmp(trimmed, "http://", 7) && strncasecmp(trimmed, "https://", 8)) {
        if (n + 8 >= sizeof(with_scheme)) return false;
        snprintf(with_scheme, sizeof(with_scheme), "https://%.*s",
                 (int)sizeof(with_scheme) - 9, trimmed);
        body = with_scheme;
    }
    if (strlen(body) >= cap) return false;
    strcpy(out, body);
    return true;
}

bool book_orbit_document_hash(const char *path, char out[33]) {
    if (!path || !out) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    orbit_md5_t m;
    md5_init(&m);
    bool ok = size > 0;
    static const long OFFSETS[] = {0, 1024, 4096, 16384, 65536, 262144, 1048576,
                                   4194304, 16777216, 67108864, 268435456, 1073741824};
    uint8_t buffer[1024];
    for (unsigned i = 0; ok && i < sizeof(OFFSETS) / sizeof(OFFSETS[0]); ++i) {
        long offset = OFFSETS[i];
        if (offset >= size) break;
        if (fseek(file, offset, SEEK_SET) != 0) continue;
        size_t take = size - offset < 1024 ? (size_t)(size - offset) : 1024;
        if (fread(buffer, 1, take, file) != take) { ok = false; break; }
        md5_update(&m, buffer, take);
    }
    fclose(file);
    if (!ok) return false;
    uint8_t digest[16];
    md5_final(&m, digest);
    md5_hex(digest, out);
    return true;
}

// ---- 路径与文件工具 ------------------------------------------------------------
static uint64_t path_hash(const char *path) {
    uint64_t h = 1469598103934665603ull;
    for (; *path; ++path) {
        h ^= (uint8_t)*path;
        h *= 1099511628211ull;
    }
    return h;
}

static bool state_file_path(const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, BOOK_ORBIT_STATE_DIR "/%s", name);
    return n > 0 && (size_t)n < cap;
}

static bool hash_file_path(const char *prefix, const char *hash, char *out, size_t cap) {
    int n = snprintf(out, cap, BOOK_ORBIT_STATE_DIR "/%s_%s.bin", prefix, hash);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_state_dir(void) {
    if (mkdir("/sdcard/.kiikoread", 0777) && errno != EEXIST) return false;
    if (mkdir(BOOK_ORBIT_STATE_DIR, 0777) && errno != EEXIST) return false;
    return true;
}

// ---- 设置快照 ------------------------------------------------------------------
typedef struct {
    char server[BOOK_ORBIT_URL_MAX];
    char user[64];
    char password[96];
} orbit_credentials_t;

static void load_credentials(orbit_credentials_t *out) {
    memset(out, 0, sizeof(*out));
    const char *server = app_settings_orbit_server();
    const char *user = app_settings_orbit_user();
    const char *password = app_settings_orbit_password();
    if (server) snprintf(out->server, sizeof(out->server), "%s", server);
    if (user) snprintf(out->user, sizeof(out->user), "%s", user);
    if (password) snprintf(out->password, sizeof(out->password), "%s", password);
    out->server[sizeof(out->server) - 1] = 0;
}

static bool credentials_ready(const orbit_credentials_t *c) {
    char normalized[BOOK_ORBIT_URL_MAX];
    return c->user[0] && c->password[0] &&
           book_orbit_normalize_server_url(c->server, normalized, sizeof(normalized));
}

// ---- 阅读事件队列（TF） ---------------------------------------------------------
typedef struct {
    uint32_t start_time;   ///< 页面开始 UTC 秒 / Page-read start, UTC epoch seconds
    uint32_t duration_s;   ///< 停留秒 / Dwell seconds
    uint16_t page_bp;      ///< 全书基点 0..10000 / Whole-book position in basis points
    uint16_t total_pages;  ///< 基点分母（自描述） / Basis-point denominator, self-describing
} orbit_event_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
} orbit_queue_header_t;

#define QUEUE_MAGIC UINT32_C(0x424F5131)
#define QUEUE_HEADER_SIZE 8

static orbit_event_t s_event_ram[BOOK_ORBIT_EVENT_RAM_MAX];
static int s_event_ram_count;
static char s_event_path[BOOK_ORBIT_EVENT_PATH_MAX];
static bool s_event_path_valid;

bool book_orbit_note_turn(const char *path, unsigned position_bp, int dir) {
    if (dir <= 0 || !path || !path[0]) return false;
    time_t now = time(NULL);
    if (now < BOOK_ORBIT_CLOCK_MIN) return false;
    /* 换书先把上一本的缓冲落盘。/ A book switch flushes the previous book's buffer first. */
    if (s_event_ram_count && (!s_event_path_valid || strcmp(s_event_path, path))) book_orbit_flush_events();
    if (s_event_ram_count == BOOK_ORBIT_EVENT_RAM_MAX) book_orbit_flush_events();
    static int64_t last_turn_us;
    int64_t now_us = esp_timer_get_time();
    uint32_t dwell = last_turn_us ? (uint32_t)((now_us - last_turn_us) / 1000000) : 0;
    last_turn_us = now_us;
    if (!s_event_path_valid) {
        int n = snprintf(s_event_path, sizeof(s_event_path), "%s", path);
        if (n <= 0 || (size_t)n >= sizeof(s_event_path)) return false;
        s_event_path_valid = true;
    }
    if (dwell > 600) dwell = 600; /* 挂起/取书时上限10分钟 */
    orbit_event_t *slot = &s_event_ram[s_event_ram_count++];
    slot->start_time = (uint32_t)now > dwell ? (uint32_t)now - dwell : 0;
    slot->duration_s = dwell ? dwell : 1;
    slot->page_bp = (uint16_t)(position_bp > 10000 ? 10000 : position_bp);
    slot->total_pages = 10000;
    return true;
}

static bool queue_append(const char *path, const orbit_event_t *events, size_t count) {
    char hash[33];
    if (!ensure_state_dir() || !book_orbit_document_hash(path, hash)) return false;
    char file_path[192];
    if (!hash_file_path("stats", hash, file_path, sizeof(file_path))) return false;
    orbit_queue_header_t header = {0};
    FILE *file = fopen(file_path, "r+b");
    if (file) {
        if (fread(&header, 1, sizeof(header), file) != sizeof(header) || header.magic != QUEUE_MAGIC) {
            fclose(file);
            file = NULL;
        }
    }
    if (!file) {
        file = fopen(file_path, "w+b");
        if (!file) return false;
        header.magic = QUEUE_MAGIC;
        header.count = 0;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < count; ++i) {
        if (header.count >= BOOK_ORBIT_QUEUE_MAX) break; /* 满队列丢新事件，保住已有积压 */
        if (fseek(file, QUEUE_HEADER_SIZE + (long)header.count * sizeof(orbit_event_t), SEEK_SET) != 0 ||
            fwrite(&events[i], 1, sizeof(orbit_event_t), file) != sizeof(orbit_event_t))
            ok = false;
        else
            ++header.count;
    }
    if (ok && fseek(file, 0, SEEK_SET) != 0) ok = false;
    if (ok && fwrite(&header, 1, sizeof(header), file) != sizeof(header)) ok = false;
    if (fclose(file) != 0) ok = false;
    return ok;
}

void book_orbit_flush_events(void) {
    if (!s_event_ram_count || !s_event_path_valid) return;
    orbit_event_t events[BOOK_ORBIT_EVENT_RAM_MAX];
    int count = s_event_ram_count;
    char path[BOOK_ORBIT_EVENT_PATH_MAX];
    memcpy(events, s_event_ram, sizeof(orbit_event_t) * count);
    memcpy(path, s_event_path, sizeof(path));
    s_event_ram_count = 0;
    s_event_path_valid = false;
    s_event_path[0] = 0;
    if (!queue_append(path, events, count)) {
        /* 落盘失败：静默丢弃本轮，避免失败重试拖住阅读器。 */
        ESP_LOGW(TAG, "session flush failed; dropping %d events", count);
    }
}

static bool queue_read_range(const char *hash, size_t first, size_t max, orbit_event_t *out, size_t *out_count) {
    char file_path[192];
    if (!hash_file_path("stats", hash, file_path, sizeof(file_path))) return false;
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        *out_count = 0;
        return true;
    }
    orbit_queue_header_t header = {0};
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) && header.magic == QUEUE_MAGIC;
    size_t available = ok && header.count > first ? header.count - first : 0;
    size_t take = available < max ? available : max;
    if (ok && take) {
        if (fseek(file, QUEUE_HEADER_SIZE + (long)first * sizeof(orbit_event_t), SEEK_SET) != 0 ||
            fread(out, sizeof(orbit_event_t), take, file) != take)
            ok = false;
    }
    fclose(file);
    if (!ok) return false;
    *out_count = take;
    return true;
}

static size_t queue_count(const char *hash) {
    char file_path[192];
    if (!hash_file_path("stats", hash, file_path, sizeof(file_path))) return 0;
    FILE *file = fopen(file_path, "rb");
    if (!file) return 0;
    orbit_queue_header_t header = {0};
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) && header.magic == QUEUE_MAGIC;
    fclose(file);
    return ok ? header.count : 0;
}

static void queue_clear(const char *hash) {
    char file_path[192];
    if (!hash_file_path("stats", hash, file_path, sizeof(file_path))) return;
    remove(file_path);
}

// ---- 高亮存储（TF，逐书一个文件） ----------------------------------------------
#define HIGHLIGHT_MAGIC UINT32_C(0x424F4831)
typedef struct {
    uint32_t magic;
    uint32_t count;
} orbit_highlight_header_t;

typedef struct {
    uint32_t server_id;
    uint32_t version;
    char datetime[20];
    uint16_t pos_len;
    uint16_t text_len;
    uint16_t chapter_len;
} orbit_highlight_record_t;

static bool highlight_file(const char *hash, char *out, size_t cap) {
    return hash_file_path("hl", hash, out, cap);
}

int book_orbit_highlight_count(const char *path) {
    char hash[33];
    char file_path[192];
    if (!path || !book_orbit_document_hash(path, hash) || !highlight_file(hash, file_path, sizeof(file_path)))
        return 0;
    FILE *file = fopen(file_path, "rb");
    if (!file) return 0;
    orbit_highlight_header_t header = {0};
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) && header.magic == HIGHLIGHT_MAGIC;
    fclose(file);
    return ok ? (int)header.count : 0;
}

static bool highlight_contains(uint32_t server_id, const char *hash) {
    char file_path[192];
    if (!highlight_file(hash, file_path, sizeof(file_path))) return false;
    FILE *file = fopen(file_path, "rb");
    if (!file) return false;
    orbit_highlight_header_t header = {0};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) || header.magic != HIGHLIGHT_MAGIC) {
        fclose(file);
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < header.count && !found; ++i) {
        orbit_highlight_record_t record = {0};
        if (fread(&record, 1, sizeof(record), file) != sizeof(record)) break;
        long skip = record.pos_len + record.text_len + record.chapter_len;
        if (record.server_id == server_id) found = true;
        else if (skip > 0 && fseek(file, skip, SEEK_CUR) != 0) break;
    }
    fclose(file);
    return found;
}

static bool highlight_append(const char *hash, uint32_t server_id, uint32_t version,
                             const char *datetime, const char *pos0, const char *text,
                             const char *chapter) {
    char file_path[192];
    if (!ensure_state_dir() || !highlight_file(hash, file_path, sizeof(file_path))) return false;
    size_t pos_len = strnlen(pos0, BOOK_ORBIT_TEXT_MAX);
    size_t text_len = strnlen(text, BOOK_ORBIT_TEXT_MAX);
    size_t chapter_len = chapter ? strnlen(chapter, 128) : 0;
    orbit_highlight_header_t header = {0};
    FILE *file = fopen(file_path, "r+b");
    if (file) {
        if (fread(&header, 1, sizeof(header), file) != sizeof(header) || header.magic != HIGHLIGHT_MAGIC) {
            fclose(file);
            file = NULL;
        }
    }
    if (!file) {
        file = fopen(file_path, "w+b");
        if (!file) return false;
        header.magic = HIGHLIGHT_MAGIC;
        header.count = 0;
    }
    bool ok = header.count < BOOK_ORBIT_HIGHLIGHT_MAX;
    if (ok) {
        orbit_highlight_record_t record;
        memset(&record, 0, sizeof(record));
        record.server_id = server_id;
        record.version = version;
        snprintf(record.datetime, sizeof(record.datetime), "%s", datetime);
        record.pos_len = (uint16_t)pos_len;
        record.text_len = (uint16_t)text_len;
        record.chapter_len = (uint16_t)chapter_len;
        ok = fseek(file, 0, SEEK_END) == 0 && fwrite(&record, 1, sizeof(record), file) == sizeof(record);
        if (ok && pos_len) ok = fwrite(pos0, 1, pos_len, file) == pos_len;
        if (ok && text_len) ok = fwrite(text, 1, text_len, file) == text_len;
        if (ok && chapter_len) ok = fwrite(chapter, 1, chapter_len, file) == chapter_len;
        if (ok) ++header.count;
    }
    if (ok) {
        ok = fseek(file, 0, SEEK_SET) == 0 && fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    }
    if (fclose(file) != 0) ok = false;
    return ok;
}

// ---- 远端进度建议（TF，路径哈希索引） -------------------------------------------
#define RESUME_MAGIC UINT32_C(0x424F5231)
#define RESUME_MAX 8
typedef struct {
    uint64_t path_hash;
    uint32_t bp;
    uint32_t epoch;
} orbit_resume_entry_t;

static bool resume_load(orbit_resume_entry_t entries[RESUME_MAX], uint32_t *count) {
    char file_path[160];
    *count = 0;
    if (!state_file_path("resume.bin", file_path, sizeof(file_path))) return false;
    FILE *file = fopen(file_path, "rb");
    if (!file) return true;
    uint32_t header[2] = {0};
    bool ok = fread(header, sizeof(uint32_t), 2, file) == 2 && header[0] == RESUME_MAGIC &&
              header[1] <= RESUME_MAX;
    if (ok && header[1]) ok = fread(entries, sizeof(orbit_resume_entry_t), header[1], file) == header[1];
    fclose(file);
    if (!ok) return false;
    *count = header[1];
    return true;
}

static bool resume_store(const orbit_resume_entry_t *entries, uint32_t count) {
    char file_path[160], temp[168];
    if (!ensure_state_dir() || !state_file_path("resume.bin", file_path, sizeof(file_path)) ||
        snprintf(temp, sizeof(temp), "%s.tmp", file_path) >= (int)sizeof(temp))
        return false;
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    uint32_t header[2] = {RESUME_MAGIC, count};
    bool ok = fwrite(header, sizeof(uint32_t), 2, file) == 2 &&
              (!count || fwrite(entries, sizeof(orbit_resume_entry_t), count, file) == count);
    if (fclose(file) != 0) ok = false;
    if (ok) ok = rename(temp, file_path) == 0;
    if (!ok) remove(temp);
    return ok;
}

static bool resume_upsert(const char *path, unsigned bp) {
    orbit_resume_entry_t entries[RESUME_MAX];
    uint32_t count = 0;
    resume_load(entries, &count);
    uint64_t key = path_hash(path);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i].path_hash == key) {
            entries[i].bp = bp;
            entries[i].epoch = (uint32_t)time(NULL);
            found = true;
        }
    if (!found && count < RESUME_MAX) {
        entries[count].path_hash = key;
        entries[count].bp = bp;
        entries[count].epoch = (uint32_t)time(NULL);
        ++count;
    }
    return resume_store(entries, count);
}

bool book_orbit_take_resume(const char *path, unsigned *out_bp) {
    orbit_resume_entry_t entries[RESUME_MAX];
    uint32_t count = 0;
    if (!path || !path[0] || !resume_load(entries, &count)) return false;
    uint64_t key = path_hash(path);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i].path_hash == key) {
            if (out_bp) *out_bp = entries[i].bp;
            found = true;
            memmove(&entries[i], &entries[i + 1], (count - i - 1) * sizeof(orbit_resume_entry_t));
            --count;
            break;
        }
    if (found) resume_store(entries, count);
    return found;
}

// ---- 同步侧记录（上次双方位置，用于冲突判定） ------------------------------------
#define SIDECAR_MAGIC UINT32_C(0x424F5331)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t server_bp;
    uint32_t local_bp;
    uint32_t epoch;
} orbit_sidecar_t;

static bool sidecar_load(const char *hash, orbit_sidecar_t *out) {
    char file_path[192];
    memset(out, 0, sizeof(*out));
    if (!hash_file_path("sync", hash, file_path, sizeof(file_path))) return false;
    FILE *file = fopen(file_path, "rb");
    if (!file) return false;
    bool ok = fread(out, 1, sizeof(*out), file) == sizeof(*out) && out->magic == SIDECAR_MAGIC &&
              out->version == 1;
    fclose(file);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static bool sidecar_store(const char *hash, uint32_t server_bp, uint32_t local_bp) {
    char file_path[192], temp[200];
    if (!ensure_state_dir() || !hash_file_path("sync", hash, file_path, sizeof(file_path)) ||
        snprintf(temp, sizeof(temp), "%s.tmp", file_path) >= (int)sizeof(temp))
        return false;
    orbit_sidecar_t record;
    memset(&record, 0, sizeof(record));
    record.magic = SIDECAR_MAGIC;
    record.version = 1;
    record.server_bp = server_bp;
    record.local_bp = local_bp;
    record.epoch = (uint32_t)time(NULL);
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    bool ok = fwrite(&record, 1, sizeof(record), file) == sizeof(record) && fclose(file) == 0;
    if (ok) ok = rename(temp, file_path) == 0;
    if (!ok) remove(temp);
    return ok;
}

// ---- 状态发布 -------------------------------------------------------------------
static book_orbit_status_t s_status;
static volatile int s_last_success_epoch;
static char s_pending_path[BOOK_ORBIT_EVENT_PATH_MAX];
static volatile bool s_pending_path_valid;
static volatile bool s_cancel;
static volatile bool s_request;

static void publish(book_orbit_state_t state, const char *message) {
    s_status.state = state;
    snprintf(s_status.message, sizeof(s_status.message), "%.*s",
             (int)sizeof(s_status.message) - 1, message ? message : "");
    if (state == BOOK_ORBIT_DONE || state == BOOK_ORBIT_FAILED || state == BOOK_ORBIT_CANCELLED)
        s_status.busy = false;
    else if (state != BOOK_ORBIT_IDLE)
        s_status.busy = true;
    ++s_status.revision;
}

static void publish_book_name(const char *path) {
    char title[64], author[64];
    if (book_display_name_from_path(path, title, sizeof(title), author, sizeof(author)))
        snprintf(s_status.book, sizeof(s_status.book), "%s", title);
    else
        s_status.book[0] = 0;
}



// ---- HTTP ----------------------------------------------------------------------
typedef struct {
    char *buf;
    size_t cap;
    size_t length;
    bool overflow;
} orbit_sink_t;

static orbit_sink_t s_sink;

static esp_err_t orbit_http_event(esp_http_client_event_t *event) {
    if (s_cancel) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA && s_sink.buf) {
        if (s_sink.length + event->data_len >= s_sink.cap) {
            s_sink.overflow = true;
            return ESP_FAIL;
        }
        memcpy(s_sink.buf + s_sink.length, event->data, event->data_len);
        s_sink.length += event->data_len;
        s_sink.buf[s_sink.length] = 0;
    }
    return ESP_OK;
}

/// KOReader 同步协议身份头；BookOrbit 的插件发送普通 application/json。
/// kosync identity headers; BookOrbit's own plugin sends plain application/json.
static bool orbit_http(const char *url, const char *method, const char *body, const orbit_credentials_t *cred,
                       char *response, size_t response_cap, int *status) {
    if (s_cancel || !url) return false;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return false;
    if (response && response_cap) response[0] = 0;
    s_sink.buf = response;
    s_sink.cap = response ? response_cap : 1;
    s_sink.length = 0;
    s_sink.overflow = false;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = orbit_http_event,
        .crt_bundle_attach = strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "x-auth-user", cred->user);
    char userkey[33];
    md5_text_hex(cred->password, strlen(cred->password), userkey);
    esp_http_client_set_header(client, "x-auth-key", userkey);
    memset(userkey, 0, sizeof(userkey));
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "kiikoread " BOOK_ORBIT_PLUGIN_VERSION);
    if (!strcmp(method, "PUT")) esp_http_client_set_method(client, HTTP_METHOD_PUT);
    else if (!strcmp(method, "POST")) esp_http_client_set_method(client, HTTP_METHOD_POST);
    else esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_post_field(client, body, body ? (int)strlen(body) : 0);
    esp_http_client_set_user_data(client, &s_sink);
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && !s_cancel) {
        ESP_LOGE(TAG, "%s %s failed: 0x%x", method, url, (unsigned)err);
        return false;
    }
    if (s_sink.overflow) {
        ESP_LOGE(TAG, "%s %s response too long", method, url);
        return false;
    }
    return true;
}

static const char *json_string(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

// ---- WiFi（照 book_xteink_sync 的按需联网路径） ---------------------------------
static bool s_owns_network;

static bool orbit_ensure_network(const char **message) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.network_ready) return true;
    char ssid[33];
    bool configured = false;
    read_pico_transfer_get_saved_wifi(ssid, &configured);
    if (!configured) {
        *message = "请先在设置中配置 WiFi";
        return false;
    }
    if (status.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
    read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    if (read_pico_transfer_start(&cfg) != ESP_OK) {
        *message = "WiFi 连接启动失败";
        return false;
    }
    s_owns_network = true;
    for (int i = 0; i < 300 && !s_cancel; ++i) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.network_ready) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    *message = s_cancel ? "已取消" : "WiFi 连接超时";
    return false;
}

static void orbit_release_network(void) {
    if (!s_owns_network) return;
    s_owns_network = false;
    read_pico_transfer_stop();
}

/// 时钟无效时借本次联网补齐，保证事件与进度时间戳可用。
/// Anchor the clock during this sync so event and progress timestamps are usable.
static void orbit_anchor_clock(void) {
    if (time(NULL) >= BOOK_ORBIT_CLOCK_MIN) return;
    uint32_t utc = 0;
    if (read_pico_transfer_sync_time_online(&utc) == ESP_OK && utc > BOOK_ORBIT_CLOCK_MIN) {
        struct timeval tv = {.tv_sec = (time_t)utc, .tv_usec = 0};
        settimeofday(&tv, NULL);
    }
}

// ---- 最近一次成功时间 ------------------------------------------------------------
static void orbit_start_worker(void);
static void last_sync_load(char out[20]) {
    char file_path[160];
    out[0] = 0;
    if (!state_file_path("lastsync.bin", file_path, sizeof(file_path))) return;
    FILE *file = fopen(file_path, "rb");
    if (!file) return;
    uint32_t epoch = 0;
    if (fread(&epoch, sizeof(epoch), 1, file) == 1 && epoch > BOOK_ORBIT_CLOCK_MIN)
        book_orbit_format_datetime(epoch, out);
    fclose(file);
}

static void last_sync_store(void) {
    char file_path[160];
    if (!ensure_state_dir() || !state_file_path("lastsync.bin", file_path, sizeof(file_path))) return;
    FILE *file = fopen(file_path, "wb");
    if (!file) return;
    uint32_t epoch = (uint32_t)time(NULL);
    fwrite(&epoch, sizeof(epoch), 1, file);
    fclose(file);
}

// ---- 电池门控（参考实现：低电量且未充电时跳过自动同步） --------------------------
static bool orbit_battery_ok(bool forced) {
    if (forced) return true;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    int percent = pmu ? pmu_battery_percent(pmu) : 100;
    if (percent >= 0 && percent < BOOK_ORBIT_BATTERY_MIN && !pmu_battery_charging(pmu)) return false;
    return true;
}

void book_orbit_request_sync(bool forced, const char *path) {
    orbit_credentials_t credentials;
    load_credentials(&credentials);
    if (!credentials_ready(&credentials)) return;
    if (!forced && !orbit_battery_ok(false)) return;
    time_t now = time(NULL);
    if (!forced && s_last_success_epoch && now - (time_t)s_last_success_epoch < BOOK_ORBIT_COOLDOWN_S) return;
    if (path && path[0]) {
        snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
        s_pending_path_valid = true;
    } else if (!s_pending_path_valid) {
        return; /* 没有可同步的书籍 */
    }
    s_cancel = false;
    s_request = true;
    orbit_start_worker();
}

void book_orbit_cancel_join(void) {
    s_cancel = true;
    for (int i = 0; i < 100 && s_status.busy; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    s_request = false;
    orbit_release_network();
}

static bool orbit_auth(const orbit_credentials_t *cred, const char *base, const char **message,
                       char *scratch, size_t scratch_cap) {
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/users/auth", base);
    int status = 0;
    if (!orbit_http(url, "GET", NULL, cred, scratch, scratch_cap, &status)) {
        *message = s_cancel ? "已取消" : "网络连接失败";
        return false;
    }
    if (status == 401) {
        *message = "BookOrbit 账号或密码错误";
        return false;
    }
    if (status != 200) {
        snprintf(scratch, scratch_cap, "验证失败 (HTTP %d)", status);
        *message = scratch;
        return false;
    }
    cJSON *root = cJSON_Parse(scratch);
    bool ok = false;
    if (root) {
        const char *authorized = json_string(root, "authorized");
        ok = !authorized || !strcmp(authorized, "OK");
        cJSON_Delete(root);
        if (ok) return true;
    }
    *message = "服务器响应无效";
    return false;
}

typedef struct {
    bool found;
    float percentage;  ///< 0..1 / 0..1
    char device[64];
    char device_id[64];
    long timestamp;
} orbit_remote_progress_t;

static bool orbit_get_progress(const orbit_credentials_t *cred, const char *base, const char *hash,
                               orbit_remote_progress_t *out, char *scratch, size_t scratch_cap) {
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/syncs/progress/%s", base, hash);
    int status = 0;
    bool transport = orbit_http(url, "GET", NULL, cred, scratch, scratch_cap, &status);
    if (!transport) return false;
    if (status == 404) return true; /* 服务器无进度 / No server-side progress */
    if (status != 200) return false;
    cJSON *root = cJSON_Parse(scratch);
    if (!root) return false;
    const cJSON *pct = cJSON_GetObjectItem(root, "percentage");
    if (cJSON_IsNumber(pct)) {
        out->found = true;
        out->percentage = (float)pct->valuedouble;
        const char *device = json_string(root, "device");
        const char *device_id = json_string(root, "device_id");
        snprintf(out->device, sizeof(out->device), "%s", device ? device : "");
        snprintf(out->device_id, sizeof(out->device_id), "%s", device_id ? device_id : "");
        const cJSON *stamp = cJSON_GetObjectItem(root, "timestamp");
        out->timestamp = cJSON_IsNumber(stamp) ? (long)stamp->valuedouble : 0;
    }
    cJSON_Delete(root);
    return true;
}

static bool orbit_put_progress(const orbit_credentials_t *cred, const char *base, const char *hash,
                               const book_progress_t *progress) {
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/syncs/progress", base);
    char device_id[32];
    orbit_device_id(device_id);
    char progress_text[48];
    snprintf(progress_text, sizeof(progress_text), "c%u.%05u", (unsigned)progress->chapter,
             (unsigned)((uint32_t)progress->pct * 100));
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "document", hash);
    cJSON_AddStringToObject(root, "progress", progress_text);
    cJSON_AddNumberToObject(root, "percentage", (double)progress->pct / 100.0);
    cJSON_AddStringToObject(root, "device", app_settings_device_name());
    cJSON_AddStringToObject(root, "device_id", device_id);
    long now = (long)time(NULL);
    if (now > 0) cJSON_AddNumberToObject(root, "timestamp", now);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;
    int status = 0;
    bool ok = orbit_http(url, "PUT", body, cred, NULL, 0, &status) && (status == 200 || status == 202);
    free(body);
    return ok;
}

static bool orbit_upload_stats(const orbit_credentials_t *cred, const char *base, const char *hash,
                               const char *device_model, size_t *uploaded) {
    size_t total = queue_count(hash);
    *uploaded = 0;
    if (!total) return true;
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/plugin/page-stats", base);
    char device_id[32];
    orbit_device_id(device_id);
    for (size_t first = 0; first < total; first += BOOK_ORBIT_STATS_BATCH) {
        orbit_event_t batch[BOOK_ORBIT_STATS_BATCH];
        size_t count = 0;
        if (!queue_read_range(hash, first, BOOK_ORBIT_STATS_BATCH, batch, &count) || !count) return false;
        cJSON *root = cJSON_CreateObject();
        if (!root) return false;
        cJSON_AddStringToObject(root, "deviceId", device_id);
        cJSON_AddStringToObject(root, "deviceModel", device_model);
        cJSON_AddStringToObject(root, "pluginVersion", BOOK_ORBIT_PLUGIN_VERSION);
        char device_time[20];
        if (book_orbit_format_datetime((uint32_t)time(NULL), device_time))
            cJSON_AddStringToObject(root, "deviceTime", device_time);
        cJSON *books = cJSON_AddArrayToObject(root, "books");
        cJSON *book = cJSON_CreateObject();
        cJSON_AddItemToArray(books, book);
        cJSON_AddStringToObject(book, "hash", hash);
        cJSON *events = cJSON_AddArrayToObject(book, "events");
        for (size_t i = 0; i < count; ++i) {
            cJSON *event = cJSON_CreateObject();
            cJSON_AddItemToArray(events, event);
            cJSON_AddNumberToObject(event, "page", batch[i].page_bp);
            cJSON_AddNumberToObject(event, "startTime", (double)batch[i].start_time);
            cJSON_AddNumberToObject(event, "durationSeconds", (double)batch[i].duration_s);
            cJSON_AddNumberToObject(event, "totalPages", batch[i].total_pages);
        }
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!body) return false;
        int status = 0;
        bool ok = orbit_http(url, "POST", body, cred, NULL, 0, &status) && status >= 200 && status < 300;
        free(body);
        if (!ok) return false;
        *uploaded += count;
    }
    queue_clear(hash);
    return true;
}

typedef struct {
    uint32_t server_id;
    uint32_t version;
    char datetime[20];
    char *pos0;
    char *text;
    char *chapter;
} orbit_incoming_t;

static int orbit_parse_incoming(const char *response, orbit_incoming_t *out, int cap, bool *unmatched,
                                bool *more) {
    *unmatched = false;
    *more = false;
    cJSON *root = cJSON_Parse(response);
    if (!root) return -1;
    int count = 0;
    const cJSON *unmatched_list = cJSON_GetObjectItem(root, "unmatched");
    for (const cJSON *hash = cJSON_IsArray(unmatched_list) ? unmatched_list->child : NULL; hash;
         hash = hash->next) {
        if (cJSON_IsString(hash) && hash->valuestring[0]) {
            *unmatched = true;
            break;
        }
    }
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    const cJSON *result = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    if (result) {
        const cJSON *more_item = cJSON_GetObjectItem(result, "more");
        const cJSON *skipped = cJSON_GetObjectItem(result, "skippedNoPosition");
        *more = cJSON_IsTrue(more_item) ||
                (cJSON_IsNumber(skipped) && skipped->valuedouble > 0);
        const cJSON *to_apply = cJSON_GetObjectItem(result, "toApply");
        const cJSON *adds = cJSON_IsObject(to_apply) ? cJSON_GetObjectItem(to_apply, "add") : NULL;
        for (const cJSON *entry = cJSON_IsArray(adds) ? adds->child : NULL; entry && count < cap;
             entry = entry->next) {
            const cJSON *id = cJSON_GetObjectItem(entry, "serverId");
            const cJSON *pos0 = cJSON_GetObjectItem(entry, "pos0");
            if (!cJSON_IsNumber(id) || !cJSON_IsString(pos0) || !pos0->valuestring[0]) continue;
            orbit_incoming_t *slot = &out[count];
            memset(slot, 0, sizeof(*slot));
            slot->server_id = (uint32_t)id->valuedouble;
            const cJSON *version = cJSON_GetObjectItem(entry, "version");
            slot->version = cJSON_IsNumber(version) ? (uint32_t)version->valuedouble : 0;
            const char *datetime = json_string(entry, "datetime");
            snprintf(slot->datetime, sizeof(slot->datetime), "%s", datetime ? datetime : "");
            slot->pos0 = strdup(pos0->valuestring);
            const char *text = json_string(entry, "text");
            slot->text = strdup(text ? text : "");
            const char *chapter = json_string(entry, "chapter");
            slot->chapter = strdup(chapter ? chapter : "");
            if (!slot->pos0 || !slot->text) {
                free(slot->pos0);
                free(slot->text);
                free(slot->chapter);
                memset(slot, 0, sizeof(*slot));
                continue;
            }
            if (strlen(slot->text) > BOOK_ORBIT_TEXT_MAX) slot->text[BOOK_ORBIT_TEXT_MAX] = 0;
            ++count;
        }
    }
    cJSON_Delete(root);
    return count;
}

static void orbit_free_incoming(orbit_incoming_t *entries, int count) {
    for (int i = 0; i < count; ++i) {
        free(entries[i].pos0);
        free(entries[i].text);
        free(entries[i].chapter);
    }
}

static bool orbit_ack(const orbit_credentials_t *cred, const char *base, const char *hash,
                      const char *device_model, const orbit_incoming_t *entries, int count) {
    if (!count) return true;
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/plugin/annotations/exchange-ack", base);
    char device_id[32];
    orbit_device_id(device_id);
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "deviceId", device_id);
    cJSON_AddStringToObject(root, "deviceModel", device_model);
    cJSON_AddStringToObject(root, "pluginVersion", BOOK_ORBIT_PLUGIN_VERSION);
    cJSON *books = cJSON_AddArrayToObject(root, "books");
    cJSON *book = cJSON_CreateObject();
    cJSON_AddItemToArray(books, book);
    cJSON_AddStringToObject(book, "hash", hash);
    cJSON *applied = cJSON_AddArrayToObject(book, "applied");
    for (int i = 0; i < count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddItemToArray(applied, entry);
        cJSON_AddNumberToObject(entry, "serverId", (double)entries[i].server_id);
        cJSON_AddNumberToObject(entry, "version", (double)entries[i].version);
        cJSON_AddStringToObject(entry, "status", "applied");
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;
    int status = 0;
    bool ok = orbit_http(url, "POST", body, cred, NULL, 0, &status) && status >= 200 && status < 300;
    free(body);
    return ok;
}

/// 高亮拉取：空键集 + keysComplete=false，服务器跳过删除处理并持续下发未确认条目；
/// 本地按 serverId 去重落地并回执确认。设备暂不产生高亮，也暂不向服务器传删除。
/// Highlight pull: an empty key set with keysComplete=false makes the server skip
/// deletion processing and keep offering unacked entries; locally we dedupe by serverId,
/// store and ack. The device mints no highlights and sends no deletions yet.
static int orbit_pull_highlights(const orbit_credentials_t *cred, const char *base, const char *hash,
                                 const char *device_model, bool *out_unmatched) {
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/plugin/annotations/exchange", base);
    char device_id[32];
    orbit_device_id(device_id);
    int total_added = 0;
    *out_unmatched = false;
    char *response = heap_caps_calloc(1, BOOK_ORBIT_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) response = heap_caps_calloc(1, BOOK_ORBIT_RESPONSE_MAX, MALLOC_CAP_8BIT);
    if (!response) return 0;
    for (int round = 0; round < 5 && !s_cancel; ++round) {
        cJSON *root = cJSON_CreateObject();
        if (!root) break;
        cJSON_AddStringToObject(root, "deviceId", device_id);
        cJSON_AddStringToObject(root, "deviceModel", device_model);
        cJSON_AddStringToObject(root, "pluginVersion", BOOK_ORBIT_PLUGIN_VERSION);
        cJSON *books = cJSON_AddArrayToObject(root, "books");
        cJSON *book = cJSON_CreateObject();
        cJSON_AddItemToArray(books, book);
        cJSON_AddStringToObject(book, "hash", hash);
        cJSON_AddBoolToObject(book, "keysComplete", false);
        cJSON_AddArrayToObject(book, "changes");
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!body) break;
        int status = 0;
        bool transport = orbit_http(url, "POST", body, cred, response, BOOK_ORBIT_RESPONSE_MAX, &status);
        free(body);
        if (!transport || status < 200 || status >= 300) break;
        orbit_incoming_t incoming[BOOK_ORBIT_STATS_BATCH];
        bool unmatched = false, more = false;
        int count = orbit_parse_incoming(response, incoming, BOOK_ORBIT_STATS_BATCH, &unmatched, &more);
        if (count < 0) break;
        if (unmatched) {
            *out_unmatched = true;
            orbit_free_incoming(incoming, count);
            break;
        }
        int applied = 0;
        for (int i = 0; i < count; ++i) {
            if (highlight_contains(incoming[i].server_id, hash)) continue;
            if (!highlight_append(hash, incoming[i].server_id, incoming[i].version, incoming[i].datetime,
                                  incoming[i].pos0, incoming[i].text, incoming[i].chapter))
                continue;
            ++applied;
        }
        if (applied) {
            orbit_ack(cred, base, hash, device_model, incoming, count);
            total_added += applied;
        }
        /* 没有新增时不确认：未能落地的高亮留在服务器待确认队列，下次重发。 */
        /* Nothing new: leave entries unacked so the server re-offers them next round. */
        orbit_free_incoming(incoming, count);
        if (!more) break;
    }
    heap_caps_free(response);
    return total_added;
}

static void orbit_sweep(const orbit_credentials_t *cred, const char *base, const char *device_model,
                        size_t stats_uploaded, size_t annotations_upserted) {
    char url[BOOK_ORBIT_URL_BUF];
    snprintf(url, sizeof(url), "%s/plugin/sweeps", base);
    char device_id[32];
    orbit_device_id(device_id);
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "deviceId", device_id);
    cJSON_AddStringToObject(root, "deviceModel", device_model);
    cJSON_AddStringToObject(root, "pluginVersion", BOOK_ORBIT_PLUGIN_VERSION);
    char device_time[20];
    if (book_orbit_format_datetime((uint32_t)time(NULL), device_time))
        cJSON_AddStringToObject(root, "deviceTime", device_time);
    cJSON_AddNumberToObject(root, "booksMatched", 1);
    cJSON_AddNumberToObject(root, "pageStatsUploaded", (double)stats_uploaded);
    cJSON_AddNumberToObject(root, "annotationsUpserted", (double)annotations_upserted);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return;
    int status = 0;
    orbit_http(url, "POST", body, cred, NULL, 0, &status);
    free(body);
}

static char s_scratch[BOOK_ORBIT_RESPONSE_MAX];

static bool orbit_run_sync(const char *path) {
    const char *message = NULL;
    orbit_credentials_t credentials;
    load_credentials(&credentials);
    char base[BOOK_ORBIT_URL_MAX + 32];
    {
        char normalized[BOOK_ORBIT_URL_MAX];
        if (!book_orbit_normalize_server_url(credentials.server, normalized, sizeof(normalized))) {
            publish(BOOK_ORBIT_FAILED, "服务器地址无效");
            return false;
        }
        snprintf(base, sizeof(base), "%s" BOOK_ORBIT_BASE_SUFFIX, normalized);
    }
    publish_book_name(path);
    publish(BOOK_ORBIT_CONNECTING, "正在连接 WiFi");
    if (!orbit_ensure_network(&message)) {
        publish(s_cancel ? BOOK_ORBIT_CANCELLED : BOOK_ORBIT_FAILED, message);
        return false;
    }
    orbit_anchor_clock();
    publish(BOOK_ORBIT_AUTH, "正在验证 BookOrbit 账号");
    if (!orbit_auth(&credentials, base, &message, s_scratch, sizeof(s_scratch))) {
        publish(s_cancel ? BOOK_ORBIT_CANCELLED : BOOK_ORBIT_FAILED, message);
        return false;
    }

    char hash[33];
    if (!book_orbit_document_hash(path, hash)) {
        publish(BOOK_ORBIT_FAILED, "无法读取书籍文件");
        return false;
    }

    char device_model[64];
    snprintf(device_model, sizeof(device_model), "%s", app_settings_device_name());

    // ---- 进度 ----
    publish(BOOK_ORBIT_PROGRESS, "正在同步阅读进度");
    struct stat file_stat;
    bool has_local = stat(path, &file_stat) == 0;
    book_progress_t local = {0};
    if (has_local) has_local = book_progress_load(path, (uint32_t)file_stat.st_size, &local);
    orbit_remote_progress_t remote = {0};
    bool remote_ok = orbit_get_progress(&credentials, base, hash, &remote, s_scratch, sizeof(s_scratch));
    if (!remote_ok) {
        orbit_release_network();
        publish(BOOK_ORBIT_FAILED, "获取云端进度失败");
        return false;
    }
    orbit_sidecar_t sidecar;
    bool have_sidecar = sidecar_load(hash, &sidecar);
    unsigned local_bp = (unsigned)local.pct * 100;
    unsigned remote_bp = remote.found ? (unsigned)(remote.percentage * 10000.0f + 0.5f) : 0;
    bool applied_remote = false;
    if (remote.found && !has_local) {
        /* 本地无进度：直接采纳云端位置。/ No local progress: adopt the cloud position. */
        resume_upsert(path, remote_bp);
        applied_remote = true;
    } else if (remote.found && has_local && remote_bp > local_bp) {
        /* 服务器更靠前：仅当本地自上次同步后未前进时应用；否则视为冲突，保守不动。 */
        /* Server ahead: apply only when local has not advanced since the last sync; any
           local advance makes this a conflict and we keep the local position. */
        if (!have_sidecar || sidecar.local_bp == local_bp) {
            resume_upsert(path, remote_bp);
            applied_remote = true;
        } else {
            ESP_LOGI(TAG, "progress conflict (%u local vs %u remote bp): keeping local", local_bp,
                     remote_bp);
        }
    } else if (has_local && (!remote.found || remote_bp < local_bp)) {
        /* 本地更靠前或服务器无进度：上传本地。/ Local ahead or nothing remote: upload local. */
        if (!orbit_put_progress(&credentials, base, hash, &local)) {
            orbit_release_network();
            publish(BOOK_ORBIT_FAILED, "上传阅读进度失败");
            return false;
        }
    }
    sidecar_store(hash, remote.found ? remote_bp : 0, local_bp);

    // ---- 阅读 session ----
    book_orbit_flush_events();
    publish(BOOK_ORBIT_STATS, "正在上传阅读 session");
    size_t stats_uploaded = 0;
    bool stats_ok = orbit_upload_stats(&credentials, base, hash, device_model, &stats_uploaded);

    // ---- 高亮 ----
    publish(BOOK_ORBIT_HIGHLIGHTS, "正在拉取云端高亮");
    bool unmatched = false;
    int highlights_added =
        stats_ok ? orbit_pull_highlights(&credentials, base, hash, device_model, &unmatched) : 0;
    if (stats_ok && !unmatched)
        orbit_sweep(&credentials, base, device_model, stats_uploaded,
                    (size_t)(highlights_added > 0 ? highlights_added : 0));

    orbit_release_network();
    if (s_cancel) {
        publish(BOOK_ORBIT_CANCELLED, "已取消");
        return false;
    }
    if (!stats_ok) {
        publish(BOOK_ORBIT_FAILED, "阅读 session 上传失败");
        return false;
    }
    char done[96];
    int n = snprintf(done, sizeof(done), "同步完成 · 高亮+%d · session %u", highlights_added,
                     (unsigned)stats_uploaded);
    if (applied_remote && n > 0 && (size_t)n < sizeof(done))
        snprintf(done + n, sizeof(done) - n, " · 已采纳云端进度");
    publish(BOOK_ORBIT_DONE, done);
    last_sync_store();
    book_orbit_format_datetime((uint32_t)time(NULL), s_status.last_sync);
    s_status.highlights = highlights_added;
    s_status.events_uploaded = (int)stats_uploaded;
    s_last_success_epoch = (int)time(NULL);
    ++s_status.revision;
    return true;
}

static volatile bool s_task_alive;
static TaskHandle_t s_task;

static void orbit_worker(void *arg) {
    (void)arg;
    for (;;) {
        if (s_request) {
            s_request = false;
            s_cancel = false;
            char path[BOOK_ORBIT_EVENT_PATH_MAX];
            snprintf(path, sizeof(path), "%s", s_pending_path);
            s_pending_path_valid = false;
            orbit_run_sync(path);
        }
        /* 空闲 60 秒后退出，下次请求重建任务。 */
        int idle_ticks = 0;
        while (!s_request && idle_ticks < BOOK_ORBIT_IDLE_TICKS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            ++idle_ticks;
        }
        if (!s_request) break;
    }
    s_task_alive = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void orbit_start_worker(void) {
    if (s_task) return;
    if (xTaskCreate(orbit_worker, "orbit_sync", 8192, NULL, 4, &s_task) == pdPASS)
        s_task_alive = true;
    else
        s_task = NULL;
}

void book_orbit_get_status(book_orbit_status_t *out) {
    orbit_credentials_t credentials;
    load_credentials(&credentials);
    memcpy(out, &s_status, sizeof(*out));
    out->configured = credentials_ready(&credentials);
    if (!s_status.last_sync[0]) last_sync_load(out->last_sync);
}
