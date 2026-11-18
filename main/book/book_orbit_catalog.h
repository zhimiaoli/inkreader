/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BookOrbit 书库目录客户端：目录分区、书籍列表、详情与文件下载。
 *      协议端点对齐 BookOrbit 官方 KOReader 插件（{server}/api/v1/koreader/plugin/catalog/…），
 *      凭据复用 KOReader 同步设置（x-auth-user / x-auth-key=MD5(密码)）。
 *      协议与存储在此，界面在 main/apps/app_book_orbit.c。
 * English: BookOrbit catalog client: root sections, book lists, detail and file download.
 *      Endpoints follow the official BookOrbit KOReader plugin
 *      ({server}/api/v1/koreader/plugin/catalog/…); credentials reuse the KOReader sync
 *      settings (x-auth-user / x-auth-key=MD5(password)). Protocol and storage live here;
 *      the UI lives in main/apps/app_book_orbit.c.
 *
 * 冻结：仅浏览与显式下载；不写阅读状态/评分；下载先写 .part 临时文件再原子改名；
 *      校验或写卡失败不落盘上架；界面通过 saved_count 通知书架重扫。
 * Frozen: browse and explicit downloads only; never writes read status or rating; downloads
 *      stage into a .part file and rename atomically; failed writes or checks never reach
 *      the shelf; the UI triggers shelf rescans via saved_count.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/// 每页书籍数量；服务端上限 100，取 6 行正好铺满一屏。
/// Books per page; the server allows up to 100, six rows fill one screen.
#define BOOK_ORBIT_CATALOG_PAGE_SIZE 6
/// 分区列表（作者/系列/图书馆…）每页条数。/ Section-list rows per page.
#define BOOK_ORBIT_CATALOG_SECTION_SIZE 8
/// 单次列表快照容量上限。/ Snapshot capacity for one list page.
#define BOOK_ORBIT_CATALOG_ITEMS_MAX 12
/// 详情页保留的可下载文件数。/ Downloadable files kept per detail.
#define BOOK_ORBIT_CATALOG_FILES_MAX 6
/// 详情简介保留字节（超长截断）。/ Detail description bytes kept (truncated beyond this).
#define BOOK_ORBIT_CATALOG_DESCRIPTION_MAX 3072

/// 书库根目录分区键（root 响应的 section 字段）。/ Root section keys (root response "section").
typedef enum {
    BOOK_ORBIT_SECTION_NONE = 0,   ///< 非分区条目 / Not a section entry
    BOOK_ORBIT_SECTION_ALL,        ///< 全部书籍 / All books
    BOOK_ORBIT_SECTION_CONTINUE,   ///< 继续阅读 / Continue reading
    BOOK_ORBIT_SECTION_RECENT,     ///< 最近添加 / Recently added
    BOOK_ORBIT_SECTION_AUTHORS,    ///< 作者 / Authors
    BOOK_ORBIT_SECTION_SERIES,     ///< 系列 / Series
    BOOK_ORBIT_SECTION_LIBRARIES,  ///< 图书馆 / Libraries
    BOOK_ORBIT_SECTION_COLLECTIONS,///< 合集 / Collections
    BOOK_ORBIT_SECTION_SCOPES,     ///< 智能视图 / Smart scopes
    BOOK_ORBIT_SECTION_OTHER,      ///< 未知分区（显示服务端标题） / Unknown (server title shown)
} book_orbit_catalog_section_t;

/// 目录/分区条目：root 与 sections 列表共用。/ Catalog entry shared by root and section lists.
typedef struct {
    char title[128];                  ///< 显示名 / Display name
    long id;                          ///< 图书馆/合集/智能视图/作者 ID，未知为 -1 / Library/collection/scope/author id, -1 when unset
    long series_id;                   ///< 系列条目的 seriesId / seriesId for series entries
    int count;                        ///< 条目下书籍数，未知为 -1 / Book count, -1 when unknown
    book_orbit_catalog_section_t kind;///< 分区类别 / Section kind
} book_orbit_catalog_item_t;

/// 书籍列表条目。/ Book list entry.
typedef struct {
    long id;                  ///< 书籍 ID / Book id
    char title[160];          ///< 书名 / Title
    char authors[128];        ///< 作者（最多前两位，逗号分隔）/ Authors (first two, comma-joined)
    char series[96];          ///< 系列名（可空）/ Series name (may be empty)
    char formats[48];         ///< 格式列表（逗号分隔）/ Formats (comma-joined)
    char read_status[16];     ///< 阅读状态原文 / Raw read status
    int progress;             ///< 阅读进度 0..100，未知 -1 / Progress 0..100, -1 unknown
} book_orbit_catalog_book_t;

/// 详情页的一个可下载文件。/ One downloadable file from the detail.
typedef struct {
    long id;              ///< 文件 ID，下载 URL 用 / File id for the download URL
    long long size_bytes; ///< 体积，未知 -1 / Size in bytes, -1 unknown
    char format[16];      ///< 格式（大写原文）/ Format (raw uppercase)
    char role[16];        ///< primary 或其他 / "primary" or others
    char variant[24];     ///< downloadVariant，空表示原文件 / downloadVariant, empty = original
    bool has_variant;     ///< 是否需要 variant 查询参数 / Whether the variant query param applies
} book_orbit_catalog_file_t;

/// 书籍详情快照。/ Book detail snapshot.
typedef struct {
    long id;                                   ///< 书籍 ID / Book id
    char title[160];                           ///< 书名 / Title
    char subtitle[96];                         ///< 副标题（可空）/ Subtitle (may be empty)
    char authors[160];                         ///< 作者（最多三位）/ Authors (up to three)
    char series[96];                           ///< 系列名 / Series name
    int series_index;                          ///< 系列序号 ×10，未知 -1 / Series index ×10, -1 unknown
    char publisher[64];                        ///< 出版社 / Publisher
    char published[24];                        ///< 出版年或日期 / Published year or date
    char language[24];                         ///< 语言 / Language
    char read_status[16];                      ///< 阅读状态 / Read status
    int progress;                              ///< 阅读进度 0..100，未知 -1 / Progress, -1 unknown
    int page_count;                            ///< 页数，未知 -1 / Page count, -1 unknown
    char description[BOOK_ORBIT_CATALOG_DESCRIPTION_MAX]; ///< 简介 / Description
    book_orbit_catalog_file_t files[BOOK_ORBIT_CATALOG_FILES_MAX]; ///< 可下载文件 / Files
    int file_count;                            ///< 文件数 / File count
} book_orbit_catalog_detail_t;

/// 书籍列表过滤条件；数值 0 表示不过滤。
/// Book list filters; zero numeric values mean "no filter".
typedef struct {
    const char* author;        ///< 作者名 / Author name
    long library_id;           ///< 图书馆 / Library
    long collection_id;        ///< 合集 / Collection
    long smart_scope_id;       ///< 智能视图 / Smart scope
    long series_id;            ///< 系列 / Series
    const char* sort;          ///< 排序键，NULL = 服务器默认 / Sort key, NULL = server default
} book_orbit_catalog_books_filter_t;

typedef enum {
    BOOK_ORBIT_CATALOG_IDLE = 0, ///< 空闲 / Idle
    BOOK_ORBIT_CATALOG_LOADING,  ///< 请求中 / Request in flight
    BOOK_ORBIT_CATALOG_DONE,     ///<
    BOOK_ORBIT_CATALOG_FAILED,   ///< 失败 / Failed
    BOOK_ORBIT_CATALOG_CANCELLED,///< 已取消 / Cancelled
} book_orbit_catalog_state_t;

typedef struct {
    book_orbit_catalog_state_t state; ///< 当前状态 / Current state
    char message[96];                 ///< 给界面的一行说明 / One-line UI message
    unsigned revision;                ///< 每次变化自增 / Incremented on every change
    bool busy;                        ///<
    long long download_total;         ///< 下载总字节，非下载时 0 / Download size, 0 when idle
    long long download_received;      ///< 已下载字节 / Bytes received
    int saved_count;                  ///< 本页会话累计成功入库 / Books saved this session
} book_orbit_catalog_status_t;

// ---- 请求（同一时刻只允许一个；忙时返回 ESP_ERR_INVALID_STATE） -----------------
// ---- Requests (one at a time; ESP_ERR_INVALID_STATE while busy) -----------------
/// 拉取目录根分区。/ Fetch the catalog root.
esp_err_t book_orbit_catalog_fetch_home(void);
/// 拉取一个分区列表（authors/series/libraries/collections/smart-scopes）。
/// Fetch one section list (authors/series/libraries/collections/smart-scopes).
esp_err_t book_orbit_catalog_fetch_section(const char* section, int page);
/// 按过滤条件拉取书籍列表。/ Fetch a book list with the given filter.
esp_err_t book_orbit_catalog_fetch_books(const book_orbit_catalog_books_filter_t* filter, int page);
/// 拉取书籍详情。/ Fetch one book's detail.
esp_err_t book_orbit_catalog_fetch_detail(long book_id);
/// 下载一个文件到书目录；file 来自最近一次详情快照。
/// Download one file into the book root; the file comes from the latest detail snapshot.
esp_err_t book_orbit_catalog_download(const char* file_name, const book_orbit_catalog_file_t* file);

bool book_orbit_catalog_busy(void);
void book_orbit_catalog_get_status(book_orbit_catalog_status_t* out);
/// 本页会话累计成功入库的书籍数；界面据此通知书架重扫。/ Books saved this session; the UI
/// triggers shelf rescans from this counter.
unsigned book_orbit_catalog_saved_count(void);
/// 取消并等待任务退出；离页、锁屏前调用。/ Cancel and join; call before leaving or locking.
void book_orbit_catalog_cancel_join(void);
/// 释放快照与状态（保留统计）。/ Drop snapshots and status (keeps counters).
void book_orbit_catalog_close(void);

// ---- 快照读取（复制语义，UI 线程随时可调） -------------------------------------
// ---- Snapshot reads (copy semantics, safe from the UI thread) -------------------
int book_orbit_catalog_item_count(void);
bool book_orbit_catalog_item_get(int index, book_orbit_catalog_item_t* out);
int book_orbit_catalog_book_count(void);
bool book_orbit_catalog_book_get(int index, book_orbit_catalog_book_t* out);
bool book_orbit_catalog_detail_get(book_orbit_catalog_detail_t* out);
/// 最近一次列表响应的分页信息；total 未知为 -1。
/// Pagination of the latest list response; total is -1 when unknown.
void book_orbit_catalog_list_range(int* page, int* total, bool* has_next);

// ---- 纯函数（宿主测试可单独编译）/ Pure helpers (host-testable) -----------------
/// 由同步设置的服务器地址推出 API 基址（归一化 + /api/v1）。
/// Derive the API base from the sync server setting (normalized + /api/v1).
bool book_orbit_catalog_api_base(const char* server, char* out, size_t cap);
/// 追加查询参数：键原样，值做 URL 百分号编码；返回新长度，失败返回 0。
/// Append a query parameter: key verbatim, value percent-encoded; returns the new
/// length, or 0 on failure.
size_t book_orbit_catalog_append_param(char* url, size_t cap, const char* key, const char* value);
/// 由书名和格式构造安全文件名：去非法字符、限长、补扩展名。
/// Build a safe file name from title and format: strip illegal characters, cap the
/// length and append the extension.
bool book_orbit_catalog_build_filename(const char* title, const char* format, char* out, size_t cap);
/// 选择详情里优先下载的文件：primary 优先，其次第一个。
/// Pick the preferred detail file: primary first, otherwise the first entry.
int book_orbit_catalog_pick_file(const book_orbit_catalog_file_t* files, int n);
