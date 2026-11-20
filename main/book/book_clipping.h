/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：阅读划线/剪藏存储。每书一个二进制文件（TF:/sdcard/.kiikoread/clippings/<路径哈希>.bin）：
 *      条目表 + 追加式文本区。锚点以源文档字节偏移为主（重排稳健），页码为回退；记
 *      layoutSignature，签名不符时只展示不渲染。文本按偏移按需读取，不整驻内存。
 *      删除为墓碑标记（同步删除传播），导出 txt/Markdown 直写 TF。
 * English: Reader highlight/clipping storage. One binary file per book
 *      (TF:/sdcard/.kiikoread/clippings/<path hash>.bin): an entry table plus an
 *      append-only text area. Anchors are source byte offsets (stable across re-layout)
 *      with page numbers as fallback; the layout signature decides renderability
 *      (signature mismatch shows but does not render). Text reads on demand by offset;
 *      deletions are tombstones (propagated to sync), and txt/Markdown export writes
 *      straight to the TF card.
 *
 * 冻结：单条文本上限 4KB，每书上限 512 条；写入走 .tmp + rename 原子提交；失败不改
 *      既有文件。设备路径可经 -DBOOK_CLIPPING_STATE_DIR 覆盖（宿主测试）。
 * Frozen: 4KB text per entry, 512 entries per book; writes commit atomically via
 *      .tmp + rename; failures never touch the existing file. The device path is
 *      overridable via -DBOOK_CLIPPING_STATE_DIR (host tests).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/// 单条剪藏文本上限（字节）。/ Text cap per entry (bytes).
#define BOOK_CLIPPING_TEXT_MAX 4096
/// 每书剪藏条目上限。/ Entry cap per book.
#define BOOK_CLIPPINGS_MAX 512

typedef struct {
    uint32_t start_offset;      ///< 主锚点：源文档字节偏移 / Primary anchor: source byte offset
    uint32_t end_offset;
    uint32_t layout_signature;  ///< 排版签名；0=不渲染仅查看 / Layout signature; 0 = view only
    uint16_t start_page;        ///< 回退锚点 / Fallback anchor
    uint16_t end_page;
    uint32_t timestamp;         ///< 创建时间（epoch 秒）/ Creation time (epoch seconds)
    uint64_t sync_revision;     ///< 最近一次同步的 revision / Last synced revision
    uint32_t text_offset;       ///< 文本在文件文本区的偏移 / Text offset inside the file
    uint32_t text_length;       ///< 文本字节数 / Text byte length
    uint16_t index;             ///< 文件内条目序号 / Entry index inside the file
    bool pending_upload;        ///< 待上传 / Awaiting upload
    bool deleted;               ///< 墓碑（待传播删除）/ Tombstone (deletion to propagate)
} book_clipping_t;

/// 载入一本书的剪藏条目表（文本不驻留内存）。/ Load one book's entry table (text stays on disk).
esp_err_t book_clipping_load(const char* book_path);
/// 同一本书已载入时不重载（同步 worker 用，避免打断阅读器内存表）；否则载入。
/// No-op when the same book's table is already loaded (used by the sync worker so the
/// reader's in-memory table is not clobbered); loads otherwise.
esp_err_t book_clipping_ensure_loaded(const char* book_path);
/// 卸载当前条目表。/ Unload the current table.
void book_clipping_unload(void);
int book_clipping_count(void);
/// 条目元数据；越界返回 false。/ Entry metadata; false when out of range.
bool book_clipping_get(int index, book_clipping_t* out);
/// 按需读取一条剪藏文本；返回写入的字节数（含 NUL 前不含）。/ Read one entry's text on
/// demand; returns the byte count excluding the NUL.
int book_clipping_text(int index, char* out, size_t cap);
/// 新增一条剪藏：截断超长、写入文本区并原子提交。/ Add one clipping: truncate overlong
/// text, append and commit atomically.
esp_err_t book_clipping_add(const char* book_path, uint32_t start_offset, uint32_t end_offset,
                            uint32_t layout_signature, int start_page, int end_page,
                            const char* text);
/// 墓碑删除（同步传播后物理压缩）。/ Tombstone removal (physically compacted after sync).
esp_err_t book_clipping_remove(int index);
/// 标记已同步并记 revision。/ Mark synced with the server revision.
esp_err_t book_clipping_mark_synced(int index, uint64_t revision);
int book_clipping_pending_count(void);
int book_clipping_deleted_count(void);

/// 导出全部剪藏：markdown=false 为纯文本，true 为 Markdown；写 TF 书库根目录
/// "剪藏-<书名>.txt/.md"。返回导出条数（不含墓碑）。
/// Export all clippings: plain text or Markdown, written next to the book as
/// "剪藏-<title>.txt/.md". Returns the exported count (tombstones excluded).
esp_err_t book_clipping_export(const char* book_path, const char* book_title, bool markdown,
                               int* exported);

// ---- 纯函数（宿主测试）/ pure helpers (host-testable) ---------------------------
/// 剪藏文件路径：<state>/clippings/<16位路径哈希>.bin。/ Clipping file path:
/// <state>/clippings/<16-hex path hash>.bin.
bool book_clipping_file_path(const char* book_path, char* out, size_t cap);
/// 导出一段：把条目文本格式化为 txt/markdown 片段。/ Format one entry for export.
bool book_clipping_format_entry(const book_clipping_t* entry, const char* text, const char* book_title,
                                bool markdown, char* out, size_t cap);
/// 文件头/条目序列化校验。/ Header/entry serialization checks.
bool book_clipping_header_valid(uint32_t magic, uint32_t version, uint32_t count, uint32_t text_used,
                                size_t file_size);
