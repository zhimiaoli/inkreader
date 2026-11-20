/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 2.0 目录客户端：JSON 目录（自描述 catalog、navigation、publications、
 *      groups 扁平化）、分页链接（next/prev）、搜索（RFC6570 子集展开）与
 *      acquisition 直链下载上架。仅支持 OPDS 2.0（application/opds+json），不做
 *      OPDS 1.x Atom 兼容。认证用 HTTP Basic（设置里的 opds 账号，可留空匿名）。
 *      协议与存储在此，界面在 main/apps/app_opds.c。
 * English: OPDS 2.0 catalog client: JSON catalogs (self-describing catalog,
 *      navigation, publications, flattened groups), pagination links (next/prev),
 *      search (RFC6570 subset expansion) and direct acquisition downloads staged
 *      onto the shelf. OPDS 2.0 (application/opds+json) only; no OPDS 1.x Atom
 *      fallback. Authentication is HTTP Basic (the opds settings account, empty =
 *      anonymous). Protocol and storage live here; the UI lives in
 *      main/apps/app_opds.c.
 *
 * 冻结：仅浏览与显式下载；下载先写 .part 临时文件再原子改名，失败不落盘上架；
 *      界面通过 saved_count 通知书架重扫；快照条目有界（12 条/页）。
 * Frozen: browse and explicit downloads only; downloads stage through .part and
 *      rename atomically; failures never reach the shelf; the UI triggers shelf
 *      rescans via saved_count; snapshot entries are bounded (12 per page).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/// 每页快照条目上限（导航条目 + 出版物）。/ Snapshot entries per page (navigation + publications).
#define BOOK_OPDS_ENTRIES_MAX 12
/// URL 缓冲长度。/ URL buffer size.
#define BOOK_OPDS_URL_MAX 224

/// 目录页的一个条目：导航链接或出版物（带首选下载链接）。/ One catalog entry: a
/// navigation link or a publication (with its preferred acquisition link).
typedef struct {
    char href[BOOK_OPDS_URL_MAX]; ///< 绝对链接：导航目标或下载直链 / Absolute: navigation target or acquisition URL
    char title[160];              ///< 显示名 / Display title
    char authors[96];             ///< 出版物作者（前两位）/ Publication authors (first two)
    char media_type[48];          ///< 首选 acquisition 的 media type / Preferred acquisition media type
    char format[16];              ///< 展示格式（EPUB/PDF…）/ Display format (EPUB/PDF/…)
    bool publication;             ///< true=出版物（可下载） / true = publication (downloadable)
} book_opds_entry_t;

typedef enum {
    BOOK_OPDS_IDLE = 0,   ///< 空闲 / Idle
    BOOK_OPDS_LOADING,    ///< 请求中 / Request in flight
    BOOK_OPDS_DONE,       ///<
    BOOK_OPDS_FAILED,     ///< 失败 / Failed
    BOOK_OPDS_CANCELLED,  ///< 已取消 / Cancelled
} book_opds_state_t;

typedef struct {
    book_opds_state_t state;          ///< 当前状态 / Current state
    char message[96];                 ///< 给界面的一行说明 / One-line UI message
    unsigned revision;                ///< 每次变化自增 / Incremented on every change
    bool busy;                        ///<
    long long download_total;         ///< 下载总字节 / Download size
    long long download_received;      ///< 已下载字节 / Bytes received
    int saved_count;                  ///< 本页会话累计成功入库 / Books saved this session
} book_opds_status_t;

// ---- 请求（同一时刻只允许一个；忙时返回 ESP_ERR_INVALID_STATE） -----------------
// ---- Requests (one at a time; ESP_ERR_INVALID_STATE while busy) -----------------
/// 打开设置中的 OPDS 服务器根地址。/ Open the OPDS server root from settings.
esp_err_t book_opds_open_home(void);
/// 打开一个目录 URL（绝对地址，来自条目或分页链接）。/ Open one catalog URL (absolute,
/// from an entry or a pagination link).
esp_err_t book_opds_open(const char* url);
/// 用当前快照的搜索模板展开关键词并请求。/ Expand the snapshot's search template and fetch.
esp_err_t book_opds_search(const char* query);
/// 请求当前页的 next/prev 分页链接。/ Follow the current page's next/prev pagination link.
esp_err_t book_opds_next_page(void);
esp_err_t book_opds_prev_page(void);
/// 下载一个条目的首选 acquisition 直链到书目录。/ Download one entry's preferred
/// acquisition link into the book root.
esp_err_t book_opds_download(int entry_index);

bool book_opds_busy(void);
void book_opds_get_status(book_opds_status_t* out);
/// 本页会话累计成功入库的书籍数；界面据此通知书架重扫。/ Books saved this session; the UI
/// triggers shelf rescans from this counter.
unsigned book_opds_saved_count(void);
/// 取消并等待任务退出；离页、锁屏前调用。/ Cancel and join; call before leaving or locking.
void book_opds_cancel_join(void);
/// 释放快照与状态（保留统计）。/ Drop snapshots and status (keeps counters).
void book_opds_close(void);

// ---- 快照读取（复制语义，UI 线程随时可调） -------------------------------------
// ---- Snapshot reads (copy semantics, safe from the UI thread) -------------------
bool book_opds_title_get(char* out, size_t cap);
int book_opds_entry_count(void);
bool book_opds_entry_get(int index, book_opds_entry_t* out);
/// 当前页分页能力。/ Pagination affordances of the current page.
void book_opds_links_get(bool* has_next, bool* has_prev, bool* has_search);
/// 当前页地址（分页/刷新用）。/ Current page URL (for paging/refresh).
bool book_opds_self_get(char* out, size_t cap);

// ---- 纯函数（宿主测试可单独编译）/ Pure helpers (host-testable) -----------------
/// 相对 href 解析成绝对 URL：绝对地址原样、//host、/path、相对路径、去掉 #fragment。
/// Resolve a possibly relative href against the base URL: absolute verbatim, //host,
/// /path, relative paths; #fragments are dropped.
bool book_opds_resolve_href(const char* base, const char* href, char* out, size_t cap);
/// 搜索模板展开：支持 {query}、{searchTerms}、{?var}、{?var=default}；无模板时追加
/// ?query=…。query 做 URL 百分号编码。
/// Expand a search template: {query}, {searchTerms}, {?var}, {?var=default}; without a
/// template append ?query=… The query is percent-encoded.
bool book_opds_expand_search(const char* templated, const char* query, char* out, size_t cap);
/// media type 转展示格式名：application/epub+zip → EPUB。/ Media type to display format.
bool book_opds_format_name(const char* media_type, char* out, size_t cap);
/// Basic 认证头：base64(user:password)。/ Basic auth header: base64(user:password).
bool book_opds_basic_auth(const char* user, const char* password, char* out, size_t cap);
/// 解析一份 OPDS 2.0 目录 JSON 到有界快照。request_url 是本次请求地址，作为相对
/// href 与 self 缺失时的解析基准。
/// Parse one OPDS 2.0 catalog JSON into a bounded snapshot. request_url is the URL of
/// this request and serves as the base for relative hrefs when self is missing.
bool book_opds_parse_catalog(const char* json, const char* request_url, char* out_title,
                             size_t title_cap, book_opds_entry_t* out_entries, int out_max,
                             int* out_count, char* out_next, char* out_prev, char* out_search,
                             char* out_self, size_t url_cap);
