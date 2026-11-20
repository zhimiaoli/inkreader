/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：book_clipping 宿主测试：文件路径、头校验、新增/去重/删除/压缩往返、文本按需
 *      读取与 txt/Markdown 导出格式。
 * English: Host tests for book_clipping: file paths, header validation, add/dedup/
 *      tombstone/compact round-trips, on-demand text reads and txt/Markdown export.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book_clipping.h"

static int s_failures;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                        \
        }                                                        \
    } while (0)

int main(void) {
    const char* book = "build/clipping-test/book.epub";
    char path[128];
    CHECK(book_clipping_file_path(book, path, sizeof(path)));
    CHECK(strstr(path, "/clippings/") && strstr(path, ".bin"));
    CHECK(!book_clipping_file_path("", path, sizeof(path)));

    CHECK(book_clipping_header_valid(0x4B4C4331, 1, 2, 10, sizeof(uint32_t) * 4 + 2 * 48 + 10));
    CHECK(!book_clipping_header_valid(0xDEAD, 1, 2, 10, 100));
    CHECK(!book_clipping_header_valid(0x4B4C4331, 2, 2, 10, 100));
    CHECK(!book_clipping_header_valid(0x4B4C4331, 1, 100000, 10, 100));

    /* 清理旧状态目录。Clean the state directory. */
    remove("build/clipping-test/clippings");
    system("rm -rf build/clipping-test && mkdir -p build/clipping-test");

    CHECK(book_clipping_load(book) == ESP_OK);
    CHECK(book_clipping_count() == 0);

    CHECK(book_clipping_add(book, 10, 40, 1234, 3, 3, "第一段划线内容") == ESP_OK);
    CHECK(book_clipping_add(book, 50, 90, 1234, 4, 5, "second highlight") == ESP_OK);
    CHECK(book_clipping_count() == 2);
    CHECK(book_clipping_pending_count() == 2);

    /* 同区间重复选择忽略。Duplicate ranges are ignored. */
    CHECK(book_clipping_add(book, 10, 40, 1234, 3, 3, "第一段划线内容") == ESP_OK);
    CHECK(book_clipping_count() == 2);

    book_clipping_t entry;
    CHECK(book_clipping_get(0, &entry));
    CHECK(entry.start_offset == 10 && entry.end_offset == 40);
    CHECK(entry.layout_signature == 1234 && entry.start_page == 3 && entry.end_page == 3);
    CHECK(entry.pending_upload && !entry.deleted);

    char text[BOOK_CLIPPING_TEXT_MAX + 1];
    CHECK(book_clipping_text(0, text, sizeof(text)) == (int)strlen("第一段划线内容"));
    CHECK(!strcmp(text, "第一段划线内容"));
    CHECK(book_clipping_text(1, text, sizeof(text)) > 0);
    CHECK(!strcmp(text, "second highlight"));
    CHECK(book_clipping_text(9, text, sizeof(text)) < 0);

    /* 超长文本截断到 UTF-8 边界且不超过上限。Overlong text truncates on a UTF-8 edge. */
    char big[BOOK_CLIPPING_TEXT_MAX + 64];
    memset(big, 0x61, sizeof(big));
    big[sizeof(big) - 1] = 0;
    CHECK(book_clipping_add(book, 100, 200, 1234, 6, 6, big) == ESP_OK);
    CHECK(book_clipping_count() == 3);
    CHECK(book_clipping_text(2, text, sizeof(text)) == BOOK_CLIPPING_TEXT_MAX);
    /* 末字节应为 ASCII（UTF-8 边界截断）。Last byte stays ASCII after the trim. */
    CHECK(text[BOOK_CLIPPING_TEXT_MAX - 1] == 'a');

    /* 删除为墓碑；未同步墓碑保留在表里。Deletion leaves a tombstone in the table. */
    CHECK(book_clipping_remove(0) == ESP_OK);
    CHECK(book_clipping_get(0, &entry) && entry.deleted && entry.pending_upload);
    CHECK(book_clipping_deleted_count() == 1);
    CHECK(book_clipping_text(0, text, sizeof(text)) > 0);

    /* 标记同步后压缩：墓碑物理清除。After syncing, the tombstone compacts away. */
    for (int i = book_clipping_count() - 1; i >= 0; --i)
        CHECK(book_clipping_mark_synced(i, 77) == ESP_OK);
    CHECK(book_clipping_count() == 2);
    CHECK(book_clipping_pending_count() == 0 && book_clipping_deleted_count() == 0);
    CHECK(book_clipping_get(0, &entry) && !entry.deleted && entry.sync_revision == 77);
    CHECK(book_clipping_text(0, text, sizeof(text)) > 0 && !strcmp(text, "second highlight"));

    /* 重载保持一致。Reload round-trips. */
    CHECK(book_clipping_load(book) == ESP_OK);
    CHECK(book_clipping_count() == 2);
    CHECK(book_clipping_text(0, text, sizeof(text)) > 0 && !strcmp(text, "second highlight"));

    /* 导出格式。Export formats. */
    CHECK(book_clipping_format_entry(&entry, "内容", "测试书", false, text, sizeof(text)));
    CHECK(strstr(text, "内容") && strstr(text, "测试书"));
    CHECK(book_clipping_format_entry(&entry, "内容", "测试书", true, text, sizeof(text)));
    CHECK(text[0] == '>' && strstr(text, "《测试书》"));

    int exported = 0;
    CHECK(book_clipping_export(book, "测试书/名字:非法", true, &exported) == ESP_OK);
    CHECK(exported == 2);
    book_clipping_unload();
    CHECK(book_clipping_count() == 0);

    if (s_failures) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }
    printf("book_clipping host tests passed\n");
    return 0;
}
