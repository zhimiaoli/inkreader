/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BookOrbit 书库页：浏览目录分区与书籍列表、查看书籍详情并显式下载到书目录。
 *      WiFi 未就绪时先复用传书页的配网界面；网络请求全部由 book_orbit_catalog 后台执行。
 * English: BookOrbit catalog page: browse sections and book lists, read book details and
 *      download explicitly into the book root. WiFi provisioning reuses the transfer page;
 *      all network work runs in book_orbit_catalog's worker.
 *
 * 冻结：不自动下载；下载完成由 saved_count 通知书架重扫；离页/锁屏先取消并等待任务。
 * Frozen: nothing downloads automatically; finished downloads notify a shelf rescan via
 *      saved_count; leaving or locking cancels and joins the worker first.
 */
#include <stdio.h>

// 网络/页面控制按体积编译，保留Pico原4MiB应用槽；协议、快照与任务时序不变。
// Size-optimize IO/page control to retain Pico's4MiB slots without changing protocol, snapshot or task timing.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include <string.h>

#include "app.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "book_orbit_catalog.h"
#include "book_store.h"
#include "read_pico_transfer.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_nav.h"

#define ORBIT_HOME_ROWS 8
#define ORBIT_HOME_TOP 196
#define ORBIT_HOME_STEP 114
#define ORBIT_LIST_ROWS 8
#define ORBIT_LIST_TOP 190
#define ORBIT_LIST_STEP 98
#define ORBIT_BOOK_ROWS 6
#define ORBIT_BOOK_TOP 190
#define ORBIT_BOOK_STEP 132
#define ORBIT_PAGER_TOP 1006
#define ORBIT_DESC_LINES 9
#define ORBIT_DETAIL_FILES 4
#define ORBIT_DESC_LINE_CAP 96

extern const app_desc_t app_transfer;

typedef enum { VIEW_HOME = 0, VIEW_SECTION, VIEW_BOOKS, VIEW_DETAIL } orbit_view_t;

typedef struct {
    orbit_view_t view;   ///< 重试目标 / Retry target
    char section[32];    ///< 分区请求参数 / Section request parameter
    int page;            ///< 列表页 / List page
    book_orbit_catalog_books_filter_t filter;  ///< 书籍列表过滤 / Book list filter
    long book_id;        ///< 详情 ID / Detail id
} orbit_request_t;

static orbit_view_t s_view = VIEW_HOME;
static orbit_view_t s_back_view = VIEW_HOME;
static orbit_request_t s_request;      ///< 当前正在加载的请求 / Request in flight
static char s_section_title[96];
static book_orbit_catalog_books_filter_t s_filter;
static char s_filter_author[136];
static int s_detail_page;
static bool s_wifi_stage, s_wifi_live;
static bool s_resume_refresh;
static unsigned s_seen_revision, s_seen_saved;
static char s_desc_lines[ORBIT_DESC_LINES][ORBIT_DESC_LINE_CAP];
static int s_desc_line_count;
static int s_desc_pages;

static EpdRect home_rect(int index) {
    return (EpdRect){36, ORBIT_HOME_TOP + index * ORBIT_HOME_STEP, 612, 96};
}
static EpdRect list_rect(int index) {
    return (EpdRect){36, ORBIT_LIST_TOP + index * ORBIT_LIST_STEP, 612, 80};
}
static EpdRect book_rect(int index) {
    return (EpdRect){36, ORBIT_BOOK_TOP + index * ORBIT_BOOK_STEP, 612, 112};
}
static EpdRect file_rect(int index) { return (EpdRect){36, 620 + index * 96, 612, 80}; }
static EpdRect download_rect(int index) {
    EpdRect row = file_rect(index);
    return (EpdRect){row.x + row.width - 170, row.y + 12, 154, 56};
}

static void fit_utf8(char* out, size_t cap, const char* in, size_t max_bytes) {
    if (!out || !cap) return;
    size_t bytes = 0;
    const unsigned char* p = (const unsigned char*)(in ? in : "");
    while (*p) {
        size_t length = *p < 0x80 ? 1 : (*p < 0xE0 ? 2 : (*p < 0xF0 ? 3 : 4));
        if (bytes + length > max_bytes || bytes + length + 1 >= cap) break;
        memcpy(out + bytes, p, length);
        bytes += length;
        p += length;
    }
    if (*p && bytes + 4 < cap) {
        memcpy(out + bytes, "…", 3);
        bytes += 3;
    }
    out[bytes] = 0;
}

static void format_size(char* out, size_t cap, long long bytes) {
    if (bytes < 0) {
        snprintf(out, cap, "未知大小");
    } else if (bytes >= 1024LL * 1024LL) {
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023) / 1024));
    }
}

static const char* home_label(book_orbit_catalog_section_t kind, const char* fallback) {
    switch (kind) {
        case BOOK_ORBIT_SECTION_ALL: return "全部书籍";
        case BOOK_ORBIT_SECTION_CONTINUE: return "继续阅读";
        case BOOK_ORBIT_SECTION_RECENT: return "最近添加";
        case BOOK_ORBIT_SECTION_AUTHORS: return "作者";
        case BOOK_ORBIT_SECTION_SERIES: return "系列";
        case BOOK_ORBIT_SECTION_LIBRARIES: return "图书馆";
        case BOOK_ORBIT_SECTION_COLLECTIONS: return "合集";
        case BOOK_ORBIT_SECTION_SCOPES: return "智能视图";
        default: return fallback && fallback[0] ? fallback : "更多";
    }
}

static const char* section_key(book_orbit_catalog_section_t kind) {
    switch (kind) {
        case BOOK_ORBIT_SECTION_AUTHORS: return "authors";
        case BOOK_ORBIT_SECTION_SERIES: return "series";
        case BOOK_ORBIT_SECTION_LIBRARIES: return "libraries";
        case BOOK_ORBIT_SECTION_COLLECTIONS: return "collections";
        case BOOK_ORBIT_SECTION_SCOPES: return "smart-scopes";
        default: return NULL;
    }
}

static const char* read_status_label(const char* status) {
    if (!status || !status[0]) return "";
    if (!strcmp(status, "read")) return "已读";
    if (!strcmp(status, "reading")) return "在读";
    if (!strcmp(status, "unread")) return "未读";
    if (!strcmp(status, "want_to_read")) return "想读";
    if (!strcmp(status, "on_hold")) return "搁置";
    if (!strcmp(status, "abandoned")) return "弃读";
    return status;
}

// 简介按显示宽度贪心换行；一次成行，渲染时直接查表。
// Greedy wrap by rendered width; wrap once and index lines while rendering.
static void wrap_description(const char* text) {
    s_desc_line_count = 0;
    s_desc_pages = 0;
    if (!text || !text[0]) return;
    const int px = 26;
    const int width = 596;
    const char* p = text;
    while (*p && s_desc_line_count < ORBIT_DESC_LINES * 8) {
        char* line = s_desc_lines[s_desc_line_count];
        size_t used = 0;
        const char* next = p;
        while (*next) {
            size_t step = ((unsigned char)*next) < 0x80 ? 1 : (((unsigned char)*next) < 0xE0   ? 2
                                                              : ((unsigned char)*next) < 0xF0 ? 3
                                                                                              : 4);
            if (used + step + 1 >= sizeof(s_desc_lines[0])) break;
            char probe[ORBIT_DESC_LINE_CAP + 4];
            memcpy(probe, line, used);
            memcpy(probe + used, next, step);
            probe[used + step] = 0;
            if (used && ui_text_fixed_width_px(px, probe) > width) break;
            memcpy(line + used, next, step);
            used += step;
            line[used] = 0;
            next += step;
        }
        if (!used) {
            /* 单字超宽也要推进，避免死循环。Advance one character even when it overflows. */
            size_t step = ((unsigned char)*p) < 0x80 ? 1 : (((unsigned char)*p) < 0xE0   ? 2
                                                           : ((unsigned char)*p) < 0xF0 ? 3
                                                                                        : 4);
            memcpy(line, p, step);
            line[step] = 0;
            used = step;
            next = p + step;
        }
        s_desc_line_count++;
        if (!*next) break;
        p = next;
        /* 下一行跳过行首空白。/ Skip leading blanks on the next line. */
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    }
    s_desc_pages = (s_desc_line_count + ORBIT_DESC_LINES - 1) / ORBIT_DESC_LINES;
    if (s_desc_pages < 1) s_desc_pages = 1;
}

// ---- WiFi 配网（照 app_cloud_sync 的复用路径） ----------------------------------
// ---- WiFi provisioning (mirrors app_cloud_sync's reuse) -------------------------
static void orbit_wifi_open(app_ctx_t* ctx) {
    s_wifi_stage = s_wifi_live = true;
    app_transfer_request_wifi_setup();
    if (app_transfer.on_enter) app_transfer.on_enter(ctx);
}
static void orbit_wifi_close(app_ctx_t* ctx) {
    if (s_wifi_live && app_transfer.on_exit) app_transfer.on_exit(ctx);
    s_wifi_live = false;
}
static bool orbit_navigation_visible(app_ctx_t* ctx) { (void)ctx; return s_wifi_stage; }
static bool orbit_present(app_ctx_t* ctx, app_redraw_t redraw) {
    return s_wifi_live && app_transfer.present && app_transfer.present(ctx, redraw);
}
static bool orbit_back_swipe_area(app_ctx_t* ctx, const ui_gesture_event_t* event) {
    return !s_wifi_live || !app_transfer.back_swipe_area || app_transfer.back_swipe_area(ctx, event);
}
static bool orbit_edge_back(app_ctx_t* ctx) {
    if (s_wifi_live && app_transfer.edge_back_enabled) return app_transfer.edge_back_enabled(ctx);
    (void)ctx;
    return true;
}
static app_redraw_t orbit_key(app_ctx_t* ctx, int key) {
    if (s_wifi_live && app_transfer.on_key) return app_transfer.on_key(ctx, key);
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

// ---- 请求 / requests ------------------------------------------------------------

static void remember_request(orbit_view_t view, const char* section, int page,
                             const book_orbit_catalog_books_filter_t* filter, long book_id) {
    s_request.view = view;
    s_request.section[0] = 0;
    if (section && section[0]) snprintf(s_request.section, sizeof(s_request.section), "%s", section);
    s_request.page = page;
    s_request.filter = filter ? *filter : (book_orbit_catalog_books_filter_t){0};
    s_request.book_id = book_id;
}

static void retry_request(void) {
    switch (s_request.view) {
        case VIEW_HOME:
            book_orbit_catalog_fetch_home();
            break;
        case VIEW_SECTION:
            book_orbit_catalog_fetch_section(s_request.section, s_request.page);
            break;
        case VIEW_BOOKS:
            book_orbit_catalog_fetch_books(&s_request.filter, s_request.page);
            break;
        case VIEW_DETAIL:
            book_orbit_catalog_fetch_detail(s_request.book_id);
            break;
    }
}

static void orbit_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_seen_revision = 0;
    s_seen_saved = book_orbit_catalog_saved_count();
    s_view = s_back_view = VIEW_HOME;
    s_detail_page = 0;
    s_wifi_stage = s_wifi_live = false;
    s_resume_refresh = false;
    orbit_wifi_open(ctx);
}

static void orbit_exit(app_ctx_t* ctx) {
    orbit_wifi_close(ctx);
    s_wifi_stage = false;
    s_resume_refresh = false;
    book_orbit_catalog_cancel_join();
    if (book_orbit_catalog_saved_count() != s_seen_saved) book_store_notify_changed();
    s_seen_saved = book_orbit_catalog_saved_count();
    s_view = s_back_view = VIEW_HOME;
}

static void orbit_media_lost(app_ctx_t* ctx) {
    orbit_wifi_close(ctx);
    s_wifi_stage = false;
    s_resume_refresh = false;
    book_orbit_catalog_cancel_join();
}

static void orbit_before_lock(app_ctx_t* ctx) {
    orbit_wifi_close(ctx);
    s_wifi_stage = true;
    s_resume_refresh = true;
    book_orbit_catalog_cancel_join();
}

// ---- 绘制 / drawing -------------------------------------------------------------

static void draw_item_row(uint8_t* fb, int index, const book_orbit_catalog_item_t* item) {
    EpdRect r = list_rect(index);
    ui_fill_round_rect(fb, r, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, 16, 0x70);
    char title[112];
    fit_utf8(title, sizeof(title), item->title, 104);
    ui_text_box(fb, (EpdRect){r.x + 20, r.y + 12, r.width - 140, 34}, 21, title, EPD_DRAW_ALIGN_LEFT, false);
    char count[24];
    if (item->count >= 0) {
        snprintf(count, sizeof(count), "%d 本", item->count);
        ui_text_box(fb, (EpdRect){r.x + r.width - 130, r.y + 12, 116, 34}, 20, count, EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void draw_book_row(uint8_t* fb, int index, const book_orbit_catalog_book_t* book) {
    EpdRect r = book_rect(index);
    ui_fill_round_rect(fb, r, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, 16, 0x70);
    char title[112];
    fit_utf8(title, sizeof(title), book->title, 104);
    ui_text_box(fb, (EpdRect){r.x + 20, r.y + 12, r.width - 40, 36}, 22, title, EPD_DRAW_ALIGN_LEFT, false);
    char sub[192];
    sub[0] = 0;
    size_t used = 0;
    if (book->authors[0]) {
        used += (size_t)snprintf(sub + used, sizeof(sub) - used, "%s", book->authors);
    }
    if (book->series[0]) {
        used += (size_t)snprintf(sub + used, sizeof(sub) - used, "%s《%s》", used ? " · " : "", book->series);
    }
    if (book->formats[0]) {
        used += (size_t)snprintf(sub + used, sizeof(sub) - used, "%s%s", used ? " · " : "", book->formats);
    }
    if (book->progress >= 0) {
        snprintf(sub + used, sizeof(sub) - used, "%s%d%%", used ? " · " : "", book->progress);
    }
    fit_utf8(sub, sizeof(sub), sub, 170);
    ui_text_box(fb, (EpdRect){r.x + 20, r.y + 62, r.width - 40, 34}, 19, sub, EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_detail_page(uint8_t* fb, const book_orbit_catalog_detail_t* detail, int page) {
    if (page == 0) {
        char title[128];
        int px = ui_text_title_fit(title, 34, 596, detail->title);
        ui_text_title_vc(fb, 342, 226, px, title, detail->title, EPD_DRAW_ALIGN_CENTER);
        int y = 262;
        if (detail->authors[0]) {
            char authors[128];
            fit_utf8(authors, sizeof(authors), detail->authors, 120);
            ui_text_box(fb, (EpdRect){44, y, 596, 30}, 20, authors, EPD_DRAW_ALIGN_LEFT, false);
            y += 34;
        }
        if (detail->series[0]) {
            char series[128];
            if (detail->series_index > 0)
                snprintf(series, sizeof(series), "系列《%s》#%d", detail->series, detail->series_index / 10);
            else
                snprintf(series, sizeof(series), "系列《%s》", detail->series);
            ui_text_box(fb, (EpdRect){44, y, 596, 30}, 20, series, EPD_DRAW_ALIGN_LEFT, false);
            y += 34;
        }
        if (detail->publisher[0] || detail->published[0] || detail->page_count > 0 || detail->language[0]) {
            char facts[160];
            facts[0] = 0;
            size_t used = 0;
            if (detail->publisher[0])
                used += (size_t)snprintf(facts + used, sizeof(facts) - used, "%s", detail->publisher);
            if (detail->published[0])
                used += (size_t)snprintf(facts + used, sizeof(facts) - used, "%s%s", used ? " · " : "", detail->published);
            if (detail->page_count > 0)
                used += (size_t)snprintf(facts + used, sizeof(facts) - used, "%s%d 页", used ? " · " : "", detail->page_count);
            if (detail->language[0])
                used += (size_t)snprintf(facts + used, sizeof(facts) - used, "%s%s", used ? " · " : "", detail->language);
            ui_text_box(fb, (EpdRect){44, y, 596, 30}, 19, facts, EPD_DRAW_ALIGN_LEFT, false);
            y += 34;
        }
        if (detail->progress >= 0) {
            char progress[64];
            snprintf(progress, sizeof(progress), "%s · %d%%", read_status_label(detail->read_status), detail->progress);
            ui_text_box(fb, (EpdRect){44, y, 596, 30}, 19, progress, EPD_DRAW_ALIGN_LEFT, false);
            y += 34;
        }
        ui_hairline(fb, y + 6, 44, 596, UI_GRAY_LIGHT);
        int files = detail->file_count < ORBIT_DETAIL_FILES ? detail->file_count : ORBIT_DETAIL_FILES;
        if (!files) {
            ui_text_vc(fb, 342, 700, 21, "没有可下载的文件", EPD_DRAW_ALIGN_CENTER, false);
        }
        for (int i = 0; i < files; ++i) {
            EpdRect r = file_rect(i);
            ui_fill_round_rect(fb, r, 14, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, r, 14, 0x70);
            char label[96];
            char size[24];
            format_size(size, sizeof(size), detail->files[i].size_bytes);
            snprintf(label, sizeof(label), "%s · %s", detail->files[i].format[0] ? detail->files[i].format : "FILE", size);
            ui_text_box(fb, (EpdRect){r.x + 18, r.y + 24, r.width - 200, 32}, 20, label, EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_button(fb, download_rect(i), "下载", false);
        }
    } else {
        int first = (page - 1) * ORBIT_DESC_LINES;
        int y = 200;
        for (int i = 0; i < ORBIT_DESC_LINES && first + i < s_desc_line_count; ++i) {
            ui_text_fixed(fb, 44, y, 26, s_desc_lines[first + i], EPD_DRAW_ALIGN_LEFT, false);
            y += 66;
        }
    }
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    if (s_wifi_live) {
        app_transfer.render(ctx, fb);
        return;
    }
    if (s_wifi_stage) {
        ui_clear_page(fb);
        ui_nav_status(fb);
        ui_text_vc(fb, 342, 430, 24, "正在准备 WiFi 连接…", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    book_orbit_catalog_status_t status;
    book_orbit_catalog_get_status(&status);

    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);

    const char* title = "BookOrbit 书库";
    if (s_view == VIEW_SECTION) title = s_section_title[0] ? s_section_title : "浏览";
    else if (s_view == VIEW_BOOKS) title = "书籍列表";
    else if (s_view == VIEW_DETAIL) title = "书籍详情";
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    if (status.message[0])
        ui_text_box(fb, (EpdRect){36, 138, 612, 36}, 20, status.message, EPD_DRAW_ALIGN_CENTER, false);

    if (status.busy) {
        if (status.download_total > 0) {
            unsigned percent = (unsigned)((uint64_t)status.download_received * 100u / (uint64_t)status.download_total);
            if (percent > 100) percent = 100;
            ui_text_vc(fb, 342, 400, 22, "正在下载…", EPD_DRAW_ALIGN_CENTER, false);
            epd_draw_rect((EpdRect){142, 460, 400, 14}, 0, fb);
            unsigned filled = (unsigned)((uint64_t)396 * percent / 100u);
            if (filled) epd_fill_rect((EpdRect){144, 462, (int)filled, 10}, 0, fb);
            char label[48];
            snprintf(label, sizeof(label), "%u%%", percent);
            ui_text_vc(fb, 342, 520, 20, label, EPD_DRAW_ALIGN_CENTER, false);
        } else {
            ui_text_vc(fb, 342, 430, 24, "加载中…", EPD_DRAW_ALIGN_CENTER, false);
        }
        if (s_view == VIEW_DETAIL && s_detail_page == 0) {
            book_orbit_catalog_detail_t detail;
            if (book_orbit_catalog_detail_get(&detail) && detail.id == s_request.book_id)
                draw_detail_page(fb, &detail, 0);
        }
        return;
    }

    if (s_view == VIEW_HOME) {
        int count = book_orbit_catalog_item_count();
        if (!count) {
            ui_text_vc(fb, 342, 430, 24, status.state == BOOK_ORBIT_CATALOG_FAILED ? "加载失败" : "目录为空",
                       EPD_DRAW_ALIGN_CENTER, false);
            ui_text_vc(fb, 342, 500, 19, status.message[0] ? status.message : "点屏幕中间重试", EPD_DRAW_ALIGN_CENTER, false);
            return;
        }
        for (int i = 0; i < count && i < ORBIT_HOME_ROWS; ++i) {
            book_orbit_catalog_item_t item;
            if (!book_orbit_catalog_item_get(i, &item)) break;
            EpdRect r = home_rect(i);
            ui_fill_round_rect(fb, r, 16, UI_GRAY_WHITE);
            ui_draw_control_frame(fb, r, 16, 0x70);
            const char* label = home_label(item.kind, item.title);
            ui_text_box(fb, (EpdRect){r.x + 20, r.y + 28, r.width - 130, 40}, 22, label, EPD_DRAW_ALIGN_LEFT, false);
            if (item.count >= 0 && item.kind != BOOK_ORBIT_SECTION_ALL && item.kind != BOOK_ORBIT_SECTION_CONTINUE &&
                item.kind != BOOK_ORBIT_SECTION_RECENT) {
                char count_text[24];
                snprintf(count_text, sizeof(count_text), "%d 本", item.count);
                ui_text_box(fb, (EpdRect){r.x + r.width - 120, r.y + 30, 104, 34}, 20, count_text, EPD_DRAW_ALIGN_RIGHT, false);
            }
        }
        return;
    }

    if (s_view == VIEW_SECTION) {
        int count = book_orbit_catalog_item_count();
        if (!count) {
            ui_text_vc(fb, 342, 430, 24, "列表为空", EPD_DRAW_ALIGN_CENTER, false);
            return;
        }
        for (int i = 0; i < count && i < ORBIT_LIST_ROWS; ++i) {
            book_orbit_catalog_item_t item;
            if (!book_orbit_catalog_item_get(i, &item)) break;
            draw_item_row(fb, i, &item);
        }
        int page, total;
        bool has_next;
        book_orbit_catalog_list_range(&page, &total, &has_next);
        int pages = has_next ? page + 1 : page;
        if (total > 0) {
            pages = (total + BOOK_ORBIT_CATALOG_SECTION_SIZE - 1) / BOOK_ORBIT_CATALOG_SECTION_SIZE;
            if (pages < 1) pages = 1;
        }
        char label[64];
        snprintf(label, sizeof(label), "%d/%d", page, pages);
        ui_pager_draw_at(fb, label, page - 1, pages, false, -1, ORBIT_PAGER_TOP);
        return;
    }

    if (s_view == VIEW_BOOKS) {
        int count = book_orbit_catalog_book_count();
        if (!count) {
            ui_text_vc(fb, 342, 430, 24, "没有书籍", EPD_DRAW_ALIGN_CENTER, false);
            return;
        }
        for (int i = 0; i < count && i < ORBIT_BOOK_ROWS; ++i) {
            book_orbit_catalog_book_t book;
            if (!book_orbit_catalog_book_get(i, &book)) break;
            draw_book_row(fb, i, &book);
        }
        int page, total;
        bool has_next;
        book_orbit_catalog_list_range(&page, &total, &has_next);
        int pages = has_next ? page + 1 : page;
        if (total > 0) {
            pages = (total + BOOK_ORBIT_CATALOG_PAGE_SIZE - 1) / BOOK_ORBIT_CATALOG_PAGE_SIZE;
            if (pages < 1) pages = 1;
        }
        char label[64];
        snprintf(label, sizeof(label), "%d/%d", page, pages);
        ui_pager_draw_at(fb, label, page - 1, pages, false, -1, ORBIT_PAGER_TOP);
        return;
    }

    // VIEW_DETAIL
    book_orbit_catalog_detail_t detail;
    if (!book_orbit_catalog_detail_get(&detail)) {
        ui_text_vc(fb, 342, 430, 24, "加载失败", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (s_detail_page == 0) wrap_description(detail.description);
    draw_detail_page(fb, &detail, s_detail_page);
    int pages = 1 + s_desc_pages;
    if (pages > 1 || s_desc_pages > 0) {
        char label[64];
        snprintf(label, sizeof(label), "%d/%d", s_detail_page + 1, pages);
        ui_pager_draw_at(fb, label, s_detail_page, pages, false, -1, ORBIT_PAGER_TOP);
    }
}

// ---- 输入 / input ---------------------------------------------------------------

static void fetch_books_page(int page) {
    remember_request(VIEW_BOOKS, NULL, page, &s_filter, 0);
    book_orbit_catalog_fetch_books(&s_filter, page);
}

static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (y < 160 && x < 120) {
        /* 返回上一视图；首页直接回到书架。Back one view; from home return to the shelf. */
        if (s_view == VIEW_HOME) {
            ctx->request_return = true;
        } else if (s_view == VIEW_DETAIL) {
            s_view = s_back_view;
            s_detail_page = 0;
        } else {
            s_view = VIEW_HOME;
        }
        return APP_REDRAW_PAGE;
    }
    book_orbit_catalog_status_t status;
    book_orbit_catalog_get_status(&status);
    if (status.busy) return APP_REDRAW_NONE;
    /* 失败时点内容区重试上一次请求。On failure, tapping the content retries the last request. */
    if (status.state == BOOK_ORBIT_CATALOG_FAILED) {
        if (ui_rect_hit((EpdRect){36, 176, 612, 880}, x, y)) {
            retry_request();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == VIEW_HOME) {
        int count = book_orbit_catalog_item_count();
        for (int i = 0; i < count && i < ORBIT_HOME_ROWS; ++i) {
            if (!ui_rect_hit(home_rect(i), x, y)) continue;
            book_orbit_catalog_item_t item;
            if (!book_orbit_catalog_item_get(i, &item)) break;
            const char* section = section_key(item.kind);
            s_filter = (book_orbit_catalog_books_filter_t){0};
            s_filter_author[0] = 0;
            s_back_view = VIEW_HOME;
            if (section) {
                snprintf(s_section_title, sizeof(s_section_title), "%s", home_label(item.kind, item.title));
                remember_request(VIEW_SECTION, section, 1, NULL, 0);
                s_view = VIEW_SECTION;
                book_orbit_catalog_fetch_section(section, 1);
            } else {
                if (item.kind == BOOK_ORBIT_SECTION_CONTINUE) s_filter.sort = "recently_read";
                else if (item.kind == BOOK_ORBIT_SECTION_RECENT) s_filter.sort = "recently_added";
                s_view = VIEW_BOOKS;
                fetch_books_page(1);
            }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == VIEW_SECTION) {
        int count = book_orbit_catalog_item_count();
        for (int i = 0; i < count && i < ORBIT_LIST_ROWS; ++i) {
            if (!ui_rect_hit(list_rect(i), x, y)) continue;
            book_orbit_catalog_item_t item;
            if (!book_orbit_catalog_item_get(i, &item)) break;
            s_filter = (book_orbit_catalog_books_filter_t){0};
            if (s_request.section[0]) {
                /* 作者用名字过滤；系列/图书馆/合集/智能视图用 ID。Authors filter by name;
                   series/libraries/collections/scopes by id. */
                if (!strcmp(s_request.section, "authors")) {
                    snprintf(s_filter_author, sizeof(s_filter_author), "%s", item.title);
                    s_filter.author = s_filter_author;
                } else if (!strcmp(s_request.section, "series")) {
                    s_filter.series_id = item.series_id > 0 ? item.series_id : item.id;
                } else if (!strcmp(s_request.section, "libraries")) {
                    s_filter.library_id = item.id;
                } else if (!strcmp(s_request.section, "collections")) {
                    s_filter.collection_id = item.id;
                } else if (!strcmp(s_request.section, "smart-scopes")) {
                    s_filter.smart_scope_id = item.id;
                }
            }
            s_back_view = VIEW_SECTION;
            s_view = VIEW_BOOKS;
            fetch_books_page(1);
            return APP_REDRAW_PAGE;
        }
        int action = ui_pager_hit_at(x, y, ORBIT_PAGER_TOP);
        if (action != UI_PAGER_NONE) {
            int page, total;
            bool has_next;
            book_orbit_catalog_list_range(&page, &total, &has_next);
            int pages = has_next ? page + 1 : page;
            if (total > 0) {
                pages = (total + BOOK_ORBIT_CATALOG_SECTION_SIZE - 1) / BOOK_ORBIT_CATALOG_SECTION_SIZE;
                if (pages < 1) pages = 1;
            }
            int target = ui_pager_target(action, page - 1, pages);
            if (target >= 0 && target != page - 1) {
                remember_request(VIEW_SECTION, s_request.section, target + 1, NULL, 0);
                book_orbit_catalog_fetch_section(s_request.section, target + 1);
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == VIEW_BOOKS) {
        int count = book_orbit_catalog_book_count();
        for (int i = 0; i < count && i < ORBIT_BOOK_ROWS; ++i) {
            if (!ui_rect_hit(book_rect(i), x, y)) continue;
            book_orbit_catalog_book_t book;
            if (!book_orbit_catalog_book_get(i, &book)) break;
            s_back_view = s_view;
            s_detail_page = 0;
            s_view = VIEW_DETAIL;
            remember_request(VIEW_DETAIL, NULL, 0, NULL, book.id);
            book_orbit_catalog_fetch_detail(book.id);
            return APP_REDRAW_PAGE;
        }
        int action = ui_pager_hit_at(x, y, ORBIT_PAGER_TOP);
        if (action != UI_PAGER_NONE) {
            int page, total;
            bool has_next;
            book_orbit_catalog_list_range(&page, &total, &has_next);
            int pages = has_next ? page + 1 : page;
            if (total > 0) {
                pages = (total + BOOK_ORBIT_CATALOG_PAGE_SIZE - 1) / BOOK_ORBIT_CATALOG_PAGE_SIZE;
                if (pages < 1) pages = 1;
            }
            int target = ui_pager_target(action, page - 1, pages);
            if (target >= 0 && target != page - 1) {
                fetch_books_page(target + 1);
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }

    // VIEW_DETAIL
    book_orbit_catalog_detail_t detail;
    if (!book_orbit_catalog_detail_get(&detail)) return APP_REDRAW_NONE;
    if (s_detail_page == 0) {
        int files = detail.file_count < ORBIT_DETAIL_FILES ? detail.file_count : ORBIT_DETAIL_FILES;
        for (int i = 0; i < files; ++i) {
            if (!ui_rect_hit(download_rect(i), x, y)) continue;
            char name[160];
            if (!book_orbit_catalog_build_filename(detail.title, detail.files[i].format, name, sizeof(name))) {
                return APP_REDRAW_NONE;
            }
            book_orbit_catalog_download(name, &detail.files[i]);
            return APP_REDRAW_PAGE;
        }
    }
    int action = ui_pager_hit_at(x, y, ORBIT_PAGER_TOP);
    if (action != UI_PAGER_NONE) {
        int pages = 1 + s_desc_pages;
        int target = ui_pager_target(action, s_detail_page, pages);
        if (target >= 0 && target != s_detail_page) {
            s_detail_page = target;
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t orbit_gesture(app_ctx_t* ctx, const ui_gesture_event_t* event) {
    if (s_wifi_live) return app_transfer.on_gesture ? app_transfer.on_gesture(ctx, event) : APP_REDRAW_NONE;
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    return action_at(ctx, event->x0, event->y0);
}

static app_redraw_t orbit_tick(app_ctx_t* ctx) {
    if (s_resume_refresh && !book_orbit_catalog_busy()) {
        s_resume_refresh = false;
        orbit_wifi_open(ctx);
        return APP_REDRAW_PAGE;
    }
    if (s_wifi_live) {
        app_redraw_t redraw = app_transfer.on_tick ? app_transfer.on_tick(ctx) : APP_REDRAW_NONE;
        if (ctx->request_return || ctx->request_app || ctx->request_menu) return redraw;
        read_pico_transfer_status_t network;
        read_pico_transfer_get_status(&network);
        if (!ctx->consumed && network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA &&
            network.state == READ_PICO_TRANSFER_READY) {
            orbit_wifi_close(ctx);
            s_wifi_stage = false;
            s_view = s_back_view = VIEW_HOME;
            remember_request(VIEW_HOME, NULL, 1, NULL, 0);
            book_orbit_catalog_fetch_home();
            return APP_REDRAW_PAGE;
        }
        return redraw;
    }
    unsigned saved = book_orbit_catalog_saved_count();
    if (saved != s_seen_saved) {
        s_seen_saved = saved;
        book_store_notify_changed();
    }
    book_orbit_catalog_status_t status;
    book_orbit_catalog_get_status(&status);
    if (status.revision == s_seen_revision) return APP_REDRAW_NONE;
    s_seen_revision = status.revision;
    return APP_REDRAW_PAGE;
}

const app_desc_t app_book_orbit = {
    .title = "BookOrbit 书库",
    .detail = "浏览 BookOrbit 书库并下载 / Browse and download from BookOrbit",
    .enter_full = true,
    .binary_ui = false,
    .persistent_navigation = true,
    .navigation_visible = orbit_navigation_visible,
    .present = orbit_present,
    .back_swipe_area = orbit_back_swipe_area,
    .on_key = orbit_key,
    .edge_back_enabled = orbit_edge_back,
    .on_enter = orbit_enter,
    .on_exit = orbit_exit,
    .on_media_lost = orbit_media_lost,
    .on_before_lock = orbit_before_lock,
    .render = render,
    .on_tick = orbit_tick,
    .on_gesture = orbit_gesture,
};
