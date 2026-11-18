/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BookOrbit（自托管书库）同步：KOReader 兼容协议客户端、阅读 session 队列、
 *      高亮拉取存储与后台自动同步任务。协议与存储在此，界面只在设置页展示状态。
 * English: BookOrbit (self-hosted library) sync: KOReader-compatible protocol client,
 *      reading-session queue, highlight pull store and the background auto-sync worker.
 *      Protocol and storage live here; only the settings page shows status.
 *
 * 冻结：凭据经设置 NVS 与 v26 备份；本地进度 NVS 只读不写（应用远端进度走待用建议，
 *      由阅读器下次开书消费）；高亮仅拉取展示（keysComplete=false 抑制双向删除）。
 * Frozen: credentials live in settings NVS and the v26 backup; local progress NVS is
 *      read-only here (remote progress lands as a resume suggestion consumed by the
 *      reader on next open); highlights are pull-only (keysComplete=false suppresses
 *      two-way deletions).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// 状态目录可被宿主测试覆盖（-DBOOK_ORBIT_STATE_DIR=...）；设备上固定在 TF 卡。
/// The state directory is overridable for host tests (-DBOOK_ORBIT_STATE_DIR=...); the
/// device always uses the TF card.
#ifndef BOOK_ORBIT_STATE_DIR
#define BOOK_ORBIT_STATE_DIR "/sdcard/.kiikoread/orbit"
#endif
/// 服务端 pluginVersion 字段上限 20 字符，超长整次上传被 HTTP 400 拒收。
/// The server caps pluginVersion at 20 chars and rejects the whole upload beyond that.
#define BOOK_ORBIT_PLUGIN_VERSION "kiikoread-bo-1"
/// 队列上限：2000 事件 × 12 字节 ≈ 24KB TF 占用；满后丢弃新事件，优先保留已有积压。
/// Queue cap: 2000 events; when full new events are dropped, keeping the existing backlog.
#define BOOK_ORBIT_QUEUE_MAX 2000
/// 每次同步上传的 session 事件批量。/ Reading-session events uploaded per request.
#define BOOK_ORBIT_STATS_BATCH 25
/// 每本书本地最多保留的拉取高亮条数。/ Pulled highlights kept per book.
#define BOOK_ORBIT_HIGHLIGHT_MAX 500
/// 高亮文本上限，与参考实现一致。/ Highlight text bound, matching the reference client.
#define BOOK_ORBIT_TEXT_MAX 2048
/// 书籍路径缓冲上限，与进度 NVS 的 287 字节路径上限一致。
/// Book-path buffer bound, matching the 287-byte progress NVS path limit.
#define BOOK_ORBIT_EVENT_PATH_MAX 288

typedef enum {
    BOOK_ORBIT_IDLE = 0,     ///< 未启动 / Not started
    BOOK_ORBIT_CONNECTING,   ///< 连接 WiFi / Connecting WiFi
    BOOK_ORBIT_AUTH,         ///< 验证账号 / Authenticating
    BOOK_ORBIT_PROGRESS,     ///< 同步进度 / Syncing progress
    BOOK_ORBIT_STATS,        ///< 上传阅读 session / Uploading sessions
    BOOK_ORBIT_HIGHLIGHTS,   ///< 拉取高亮 / Pulling highlights
    BOOK_ORBIT_DONE,         ///< 本轮完成 / Round finished
    BOOK_ORBIT_FAILED,       ///< 失败 / Failed
    BOOK_ORBIT_CANCELLED,    ///< 已取消 / Cancelled
} book_orbit_state_t;

typedef struct {
    book_orbit_state_t state; ///< 当前状态 / Current state
    char message[96];         ///< 给设置页的一行说明 / One-line settings message
    char book[96];            ///< 最近同步的书籍名 / Last synced book name
    char last_sync[20];       ///< 最近成功时间 "YYYY-MM-DD HH:MM:SS" / Last success time
    int highlights;           ///< 最近一次拉取新增高亮 / Highlights added last round
    int events_uploaded;      ///< 最近一次上传 session 事件 / Session events uploaded last round
    unsigned revision;        ///< 每次变化自增 / Incremented on every change
    bool busy;                ///< 后台任务在跑 / Worker running
    bool configured;          ///< 服务器/账号/密码齐备 / Server, user and password all set
} book_orbit_status_t;

/// 记录一次向前翻页：bookOrbit page-stats 事件（基点位置 + 停留秒）。
/// RAM 缓冲，凑满或退出书籍时成批落盘；时钟无效时丢弃。dir>0 才记录。
/// Record one forward page turn as a page-stats event (basis-point position plus dwell
/// seconds). Buffered in RAM and flushed as a batch when full or on book exit; dropped
/// while the clock is invalid. Only dir > 0 records.
/// 返回是否成功入队（目录无效、时钟无效或 dir<=0 时为 false）。
/// Returns whether the event was queued (false for dir<=0, invalid clock or bad path).
bool book_orbit_note_turn(const char* path, unsigned position_bp, int dir);

/// 把 RAM 缓冲的 session 事件成批写入 TF 队列；退出书籍时调用。
/// Flush buffered session events to the TF queue as one batch; call on book exit.
void book_orbit_flush_events(void);

/// 请求一次后台同步。forced=false（翻页触发）受冷却与低电量限制；true 立即执行。
/// Request a background sync. forced=false (page-turn trigger) honours the cooldown and
/// low-battery gate; true runs immediately.
void book_orbit_request_sync(bool forced, const char* path);

/// 取消并等待任务退出，释放临时网络；锁屏/关机前调用。
/// Cancel and join the worker, releasing the temporary network; call before sleep/off.
void book_orbit_cancel_join(void);

/// 设置页状态快照。/ Status snapshot for the settings page.
void book_orbit_get_status(book_orbit_status_t* out);

/// 指定路径的高亮条数（读文件，不碰网络）。/ Highlight count for a path (file read, no network).
int book_orbit_highlight_count(const char* path);

/// 阅读器开书时消费远端进度建议；有则输出全书基点位置并清除该条。
/// Consume a pending remote-progress suggestion when opening a book; outputs the
/// whole-book basis-point position and clears the entry when present.
bool book_orbit_take_resume(const char* path, unsigned* out_bp);

// ---- 纯函数（宿主测试可单独编译）/ Pure helpers (host-testable) -----------------
/// 归一化服务器地址：裸主机名补 https://，剥离 /api/v1 与 /api/v1/koreader 后缀。
/// Normalize a server URL: bare hosts gain https://, /api/v1 and /api/v1/koreader suffixes are stripped.
bool book_orbit_normalize_server_url(const char* in, char* out, size_t cap);

/// "YYYY-MM-DD HH:MM:SS"（UTC 语义，不做时区解释，往返字节一致）。
/// "YYYY-MM-DD HH:MM:SS" (UTC semantics, no timezone interpretation; round-trips byte-equal).
bool book_orbit_format_datetime(uint32_t epoch_seconds, char out[20]);
bool book_orbit_parse_datetime(const char* datetime, uint32_t* out_epoch_seconds);

/// 服务端高亮身份键 md5(datetime | pos0)，32 个小写十六进制字符加终止符。
/// Server identity key md5(datetime | pos0), 32 lowercase hex characters plus terminator.
bool book_orbit_annotation_key(const char* datetime, const char* pos0, char out[33]);

/// KOReader 认证 userkey = md5(密码)，33 字节（32 hex + 终止符）。
/// KOReader auth userkey = md5(password), 33 bytes (32 hex plus terminator).
bool book_orbit_userkey(const char* password, char out[33]);

/// KOReader 部分 MD5 文档哈希：偏移 0,1024,4096,...,2^30 各读 1024 字节拼接求 MD5。
/// KOReader partial-MD5 document hash: 1024 bytes at offsets 0,1024,4096,...,2^30.
/// out 接收 33 字节（32 hex + 终止符）；文件不可读返回 false。
/// out receives 33 bytes (32 hex + terminator); false when unreadable.
bool book_orbit_document_hash(const char* path, char out[33]);
