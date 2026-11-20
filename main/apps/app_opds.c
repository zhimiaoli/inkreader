/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 2.0 书库页：浏览目录（导航/出版物混合列表）、翻页、搜索并显式下载到
 *      书目录。WiFi 未就绪时先复用传书页的配网界面；网络请求全部由 book_opds 后台执行。
 * English: OPDS 2.0 catalog page: browse catalogs (mixed navigation/publication list),
 *      paginate, search and download explicitly into the book root. WiFi provisioning
 *      reuses the transfer page; all network work runs in book_opds's worker.
 *
 * 冻结：不自动下载；下载完成由 saved_count 通知书架重扫；离页/锁屏先取消并等待任务；
 *      导航栈最多 8 层。
 * Frozen: nothing downloads automatically; finished downloads notify a shelf rescan via
 *      saved_count; leaving or locking cancels and joins the worker first; the navigation
 *      stack is capped at 8 levels.
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
#include "book_opds.h"
#include "book_store.h"
#include "read_pico_transfer.h"
#include "ui_gesture.h"
#include "ui_keyboard.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_root_style.h"

#define OPDS_ROWS 6
#define OPDS_ROW_TOP 190
#define OPDS_ROW_STEP 132
#define OPDS_PAGER_TOP 1010
#define OPDS_NAV_STACK 8

extern const app_desc_t app_transfer;

typedef enum { VIEW_LIST = 0, VIEW_SEARCH } opds_view_t;

static opds_view_t s_view = VIEW_LIST;
static char s_history[OPDS_NAV_STACK][BOOK_OPDS_URL_MAX]; ///< 导航返回栈 / Navigation back stack
static int s_history_depth;
static char s_query[96];
static char s_query_draft[96];
static bool s_wifi_stage, s_wifi_live;
static bool s_resume_refresh;
static unsigned s_seen_revision, s_seen_saved;

static EpdRect row_rect(int index) {
    return (EpdRect){36, OPDS_ROW_TOP + index * OPDS_ROW_STEP, 612, 112};
}
static EpdRect prev_button_rect(void) { return (EpdRect){54, OPDS_PAGER_TOP, 278, 78}; }
static EpdRect next_button_rect(void) { return (EpdRect){352, OPDS_PAGER_TOP, 278, 78}; }

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

// ---- WiFi 配网（照 app_book_orbit 的复用路径） ----------------------------------
// ---- WiFi provisioning (mirrors app_book_orbit's reuse) -------------------------
static void opds_wifi_open(app_ctx_t* ctx) {
    s_wifi_stage = s_wifi_live = true;
    app_transfer_request_wifi_setup();
    if (app_transfer.on_enter) app_transfer.on_enter(ctx);
}
static void opds_wifi_close(app_ctx_t* ctx) {
    if (s_wifi_live && app_transfer.on_exit) app_transfer.on_exit(ctx);
    s_wifi_live = false;
}
static bool opds_navigation_visible(app_ctx_t* ctx) { (void)ctx; return s_wifi_stage; }
static bool opds_present(app_ctx_t* ctx, app_redraw_t redraw) {
    return s_wifi_live && app_transfer.present && app_transfer.present(ctx, redraw);
}
static bool opds_back_swipe_area(app_ctx_t* ctx, const ui_gesture_event_t* event) {
    return !s_wifi_live || !app_transfer.back_swipe_area || app_transfer.back_swipe_area(ctx, event);
}
static bool opds_edge_back(app_ctx_t* ctx) {
    if (s_wifi_live && app_transfer.edge_back_enabled) return app_transfer.edge_back_enabled(ctx);
    (void)ctx;
    return true;
}
static app_redraw_t opds_key(app_ctx_t* ctx, int key) {
    if (s_wifi_live && app_transfer.on_key) return app_transfer.on_key(ctx, key);
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static void opds_enter(app_ctx_t* ctx) {
    (void)ctx;
    s_seen_revision = 0;
    s_seen_saved = book_opds_saved_count();
    s_view = VIEW_LIST;
    s_history_depth = 0;
    s_query[0] = 0;
    s_wifi_stage = s_wifi_live = false;
    s_resume_refresh = false;
    opds_wifi_open(ctx);
}

static void opds_exit(app_ctx_t* ctx) {
    opds_wifi_close(ctx);
    s_wifi_stage = false;
    s_resume_refresh = false;
    ui_keyboard_end();
    book_opds_cancel_join();
    if (book_opds_saved_count() != s_seen_saved) book_store_notify_changed();
    s_seen_saved = book_opds_saved_count();
    s_view = VIEW_LIST;
    s_history_depth = 0;
}

static void opds_media_lost(app_ctx_t* ctx) {
    opds_wifi_close(ctx);
    s_wifi_stage = false;
    s_resume_refresh = false;
    ui_keyboard_end();
    book_opds_cancel_join();
}

static void opds_before_lock(app_ctx_t* ctx) {
    opds_wifi_close(ctx);
    s_wifi_stage = true;
    s_resume_refresh = true;
    ui_keyboard_end();
    book_opds_cancel_join();
}

// ---- 绘制 / drawing -------------------------------------------------------------

static void draw_entry_row(uint8_t* fb, int index, const book_opds_entry_t* entry) {
    EpdRect r = row_rect(index);
    ui_fill_round_rect(fb, r, 16, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, 16, 0x70);
    char title[112];
    fit_utf8(title, sizeof(title), entry->title, 104);
    ui_text_box(fb, (EpdRect){r.x + 20, r.y + 12, r.width - 140, 36}, 22, title, EPD_DRAW_ALIGN_LEFT, false);
    char sub[160];
    if (entry->publication) {
        snprintf(sub, sizeof(sub), "%s%s%s%s%s", entry->authors[0] ? entry->authors : "",
                 entry->authors[0] && entry->format[0] ? " · " : "",
                 entry->format[0] ? entry->format : "",
                 entry->href[0] ? (entry->authors[0] || entry->format[0] ? " · " : "") : "",
                 entry->href[0] ? "可下载" : "无直链");
    } else {
        snprintf(sub, sizeof(sub), "目录");
    }
    fit_utf8(sub, sizeof(sub), sub, 150);
    ui_text_box(fb, (EpdRect){r.x + 20, r.y + 62, r.width - 40, 34}, 19, sub, EPD_DRAW_ALIGN_LEFT, false);
    if (entry->publication && entry->href[0]) {
        ui_draw_button(fb, (EpdRect){r.x + r.width - 110, r.y + 24, 92, 62}, "下载", true);
    }
}

// 搜索视图需要一个文本编辑器（与 app_files 的搜索视图同款）。
// The search view needs a text editor (same pattern as app_files's search view).
static ui_text_edit_t s_editor_input;

static void render_search(uint8_t* fb) {
    ui_nav_back(fb, 36, 79);
    ui_text_box(fb, (EpdRect){120, 79, 380, 66}, 34, "OPDS 搜索", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_box(fb, (EpdRect){530, 79, 118, 66}, 26, "应用", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_input_draw(fb, &s_editor_input, (EpdRect){36, 242, 612, 82}, 29, false, NULL);
    ui_text_box(fb, (EpdRect){36, 352, 612, 84}, 22, "用目录提供的搜索接口查询；中文与英文都可以",
                EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, (EpdRect){36, 458, 180, 66}, "清空", false);
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (s_view == VIEW_SEARCH) {
        render_search(fb);
        return;
    }
    ui_root_background(fb);
    ui_nav_back(fb, 36, 79);
    char title[80];
    book_opds_title_get(title, sizeof(title));
    if (!title[0]) snprintf(title, sizeof(title), "OPDS 书库");
    ui_text_box(fb, (EpdRect){120, 79, 400, 66}, 30, title, EPD_DRAW_ALIGN_CENTER, false);
    book_opds_status_t status;
    book_opds_get_status(&status);
    bool has_next, has_prev, has_search;
    book_opds_links_get(&has_next, &has_prev, &has_search);
    if (has_search) ui_draw_button(fb, (EpdRect){530, 84, 118, 56}, "搜索", !status.busy);
    int count = book_opds_entry_count();
    if (status.busy) {
        char message[128];
        if (status.download_total > 0)
            snprintf(message, sizeof(message), "下载中 %u%%",
                     (unsigned)((uint64_t)status.download_received * 100u / (uint64_t)status.download_total));
        else
            snprintf(message, sizeof(message), "%s", status.message[0] ? status.message : "正在加载…");
        ui_text_vc(fb, 342, 430, 24, message, EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (status.state == BOOK_OPDS_FAILED) {
        ui_text_vc(fb, 342, 400, 24, status.message, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 460, 19, "点内容区重试", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    if (!count) {
        ui_text_vc(fb, 342, 430, 24, "目录为空", EPD_DRAW_ALIGN_CENTER, false);
        return;
    }
    for (int i = 0; i < count && i < OPDS_ROWS; ++i) {
        book_opds_entry_t entry;
        if (!book_opds_entry_get(i, &entry)) break;
        draw_entry_row(fb, i, &entry);
    }
    ui_draw_button(fb, prev_button_rect(), "上一页", has_prev && !status.busy);
    ui_draw_button(fb, next_button_rect(), "下一页", has_next && !status.busy);
}

// ---- 输入 / input ---------------------------------------------------------------

static app_redraw_t keyboard_paint(app_ctx_t* ctx, bool field) {
    ui_keyboard_update_t update = ui_keyboard_update(ctx->fb, UI_SYSTEM_KEYBOARD_TOP,
                                                     (EpdRect){36, 242, 612, 82}, 29, false, NULL, field);
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

static app_redraw_t search_apply(void) {
    snprintf(s_query, sizeof(s_query), "%s", s_query_draft);
    ui_keyboard_end();
    s_view = VIEW_LIST;
    if (!s_query[0]) return APP_REDRAW_PAGE;
    book_opds_search(s_query);
    return APP_REDRAW_PAGE;
}

static void remember_current(void) {
    if (s_history_depth >= OPDS_NAV_STACK) {
        memmove(s_history, s_history + 1, sizeof(s_history[0]) * (OPDS_NAV_STACK - 1));
        s_history_depth = OPDS_NAV_STACK - 1;
    }
    book_opds_self_get(s_history[s_history_depth], sizeof(s_history[0]));
    s_history_depth++;
}

static bool back_current(char* out, size_t cap) {
    if (!s_history_depth) return false;
    s_history_depth--;
    snprintf(out, cap, "%s", s_history[s_history_depth]);
    return out[0] != 0;
}

static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    (void)ctx;
    if (y < 160 && x < 120) {
        if (s_history_depth) {
            char url[BOOK_OPDS_URL_MAX];
            if (back_current(url, sizeof(url))) {
                book_opds_open(url);
                return APP_REDRAW_PAGE;
            }
        }
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    book_opds_status_t status;
    book_opds_get_status(&status);
    if (status.busy) return APP_REDRAW_NONE;
    if (ui_rect_hit((EpdRect){530, 84, 118, 56}, x, y)) {
        snprintf(s_query_draft, sizeof(s_query_draft), "%s", s_query);
        ui_text_edit_init(&s_editor_input, s_query_draft, sizeof(s_query_draft));
        ui_keyboard_begin(&s_editor_input, false);
        s_view = VIEW_SEARCH;
        return APP_REDRAW_PAGE;
    }
    /* 失败时点内容区重试当前页。On failure, tapping the content reloads the current page. */
    if (status.state == BOOK_OPDS_FAILED) {
        char url[BOOK_OPDS_URL_MAX];
        if (ui_rect_hit((EpdRect){36, 176, 612, 800}, x, y) && book_opds_self_get(url, sizeof(url))) {
            book_opds_open(url);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(next_button_rect(), x, y))
        return book_opds_next_page() == ESP_OK ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    if (ui_rect_hit(prev_button_rect(), x, y))
        return book_opds_prev_page() == ESP_OK ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    int count = book_opds_entry_count();
    for (int i = 0; i < count && i < OPDS_ROWS; ++i) {
        EpdRect r = row_rect(i);
        if (!ui_rect_hit(r, x, y)) continue;
        book_opds_entry_t entry;
        if (!book_opds_entry_get(i, &entry)) break;
        if (entry.publication) {
            /* 出版物行只走下载按钮；行本体不做详情页。Publication rows only download. */
            if (entry.href[0] && ui_rect_hit((EpdRect){r.x + r.width - 110, r.y + 24, 92, 62}, x, y)) {
                book_opds_download(i);
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        remember_current();
        book_opds_open(entry.href);
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t opds_gesture(app_ctx_t* ctx, const ui_gesture_event_t* event) {
    if (s_wifi_live) return app_transfer.on_gesture ? app_transfer.on_gesture(ctx, event) : APP_REDRAW_NONE;
    if (s_view == VIEW_SEARCH) {
        bool feedback = ui_keyboard_release();
        if (event->type != UI_GESTURE_TAP) return feedback ? keyboard_paint(ctx, false) : APP_REDRAW_NONE;
        int x = event->x0, y = event->y0;
        if (y < 160 && x < 120) { ui_keyboard_end(); s_view = VIEW_LIST; return APP_REDRAW_PAGE; }
        if (y < 160 && x > 510) return search_apply();
        if (ui_rect_hit((EpdRect){36, 458, 180, 66}, x, y)) {
            s_query_draft[0] = 0;
            ui_text_edit_init(&s_editor_input, s_query_draft, sizeof(s_query_draft));
            return APP_REDRAW_PAGE;
        }
        if (y >= 160 && y < UI_SYSTEM_KEYBOARD_TOP) {
            if (ui_text_input_tap(&s_editor_input, (EpdRect){36, 242, 612, 82}, 29, false, NULL, x, y))
                return keyboard_paint(ctx, true);
            return APP_REDRAW_NONE;
        }
        ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms);
        if (result == UI_KEYBOARD_DONE) return search_apply();
        return result == UI_KEYBOARD_CHANGED || feedback ? keyboard_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    return action_at(ctx, event->x0, event->y0);
}

static app_redraw_t opds_tick(app_ctx_t* ctx) {
    if (s_resume_refresh && !book_opds_busy()) {
        s_resume_refresh = false;
        opds_wifi_open(ctx);
        return APP_REDRAW_PAGE;
    }
    if (s_wifi_live) {
        app_redraw_t redraw = app_transfer.on_tick ? app_transfer.on_tick(ctx) : APP_REDRAW_NONE;
        if (ctx->request_return || ctx->request_app || ctx->request_menu) return redraw;
        read_pico_transfer_status_t network;
        read_pico_transfer_get_status(&network);
        if (!ctx->consumed && network.network_ready && network.mode == READ_PICO_TRANSFER_MODE_STA &&
            network.state == READ_PICO_TRANSFER_READY) {
            opds_wifi_close(ctx);
            s_wifi_stage = false;
            s_view = VIEW_LIST;
            s_history_depth = 0;
            book_opds_open_home();
            return APP_REDRAW_PAGE;
        }
        return redraw;
    }
    unsigned saved = book_opds_saved_count();
    if (saved != s_seen_saved) {
        s_seen_saved = saved;
        book_store_notify_changed();
    }
    book_opds_status_t status;
    book_opds_get_status(&status);
    if (status.revision == s_seen_revision) return APP_REDRAW_NONE;
    s_seen_revision = status.revision;
    return APP_REDRAW_PAGE;
}

const app_desc_t app_opds = {
    .title = "OPDS 书库",
    .detail = "浏览 OPDS 2.0 书库并下载 / Browse and download from OPDS 2.0",
    .enter_full = true,
    .binary_ui = false,
    .persistent_navigation = true,
    .navigation_visible = opds_navigation_visible,
    .present = opds_present,
    .back_swipe_area = opds_back_swipe_area,
    .on_key = opds_key,
    .edge_back_enabled = opds_edge_back,
    .on_enter = opds_enter,
    .on_exit = opds_exit,
    .on_media_lost = opds_media_lost,
    .on_before_lock = opds_before_lock,
    .render = render,
    .on_tick = opds_tick,
    .on_gesture = opds_gesture,
};
