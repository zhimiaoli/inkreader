/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：book_orbit_catalog 纯函数宿主测试：API 基址、查询参数编码、下载文件名与文件挑选。
 * English: Host tests for book_orbit_catalog pure helpers: API base, query parameter
 *      encoding, download file names and file picking.
 */
#include <stdio.h>
#include <string.h>

#include "book_orbit_catalog.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

static void test_api_base(void) {
    char out[256];
    CHECK(book_orbit_catalog_api_base("http://192.168.1.10:8080", out, sizeof(out)));
    CHECK(!strcmp(out, "http://192.168.1.10:8080/api/v1"));
    CHECK(book_orbit_catalog_api_base("https://books.example.com/api/v1/koreader", out, sizeof(out)));
    CHECK(!strcmp(out, "https://books.example.com/api/v1"));
    CHECK(book_orbit_catalog_api_base("books.example.com", out, sizeof(out)));
    CHECK(!strcmp(out, "https://books.example.com/api/v1"));
    CHECK(!book_orbit_catalog_api_base("", out, sizeof(out)));
}

static void test_append_param(void) {
    char url[256];
    snprintf(url, sizeof(url), "%s", "https://x/api/v1/koreader/plugin/catalog/books?page=1");
    CHECK(book_orbit_catalog_append_param(url, sizeof(url), "author", "鲁迅"));
    CHECK(!strcmp(url, "https://x/api/v1/koreader/plugin/catalog/books?page=1&author=%E9%B2%81%E8%BF%85"));
    CHECK(book_orbit_catalog_append_param(url, sizeof(url), "q", "a b&c"));
    CHECK(!strcmp(url, "https://x/api/v1/koreader/plugin/catalog/books?page=1&author=%E9%B2%81%E8%BF%85&q=a%20b%26c"));
    /* 键只允许未保留字符；非法键拒绝。Keys must be unreserved; invalid keys are rejected. */
    char bad[64];
    snprintf(bad, sizeof(bad), "%s", "https://x");
    CHECK(!book_orbit_catalog_append_param(bad, sizeof(bad), "a b", "v"));
    /* 空值跳过。Empty values are skipped. */
    CHECK(!book_orbit_catalog_append_param(bad, sizeof(bad), "q", ""));
}

static void test_build_filename(void) {
    char name[192];
    CHECK(book_orbit_catalog_build_filename("三体", "EPUB", name, sizeof(name)));
    CHECK(!strcmp(name, "三体.epub"));
    CHECK(book_orbit_catalog_build_filename("a/b:c*d", "PDF", name, sizeof(name)));
    CHECK(!strcmp(name, "a_b_c_d.pdf"));
    CHECK(book_orbit_catalog_build_filename("  书名。 ", "", name, sizeof(name)));
    CHECK(!strcmp(name, "书名。.epub"));
    CHECK(book_orbit_catalog_build_filename("", "", name, sizeof(name)));
    CHECK(!strcmp(name, "book.epub"));
    /* 长书名截断但保留扩展名。Long titles truncate while keeping the extension. */
    char long_title[300];
    for (size_t i = 0; i < 299; ++i) long_title[i] = (char)('a' + (i % 26));
    long_title[299] = 0;
    CHECK(book_orbit_catalog_build_filename(long_title, "EPUB", name, sizeof(name)));
    size_t length = strlen(name);
    CHECK(length < sizeof(name));
    CHECK(!strcmp(name + length - 5, ".epub"));
}

static void test_pick_file(void) {
    book_orbit_catalog_file_t files[3];
    memset(files, 0, sizeof(files));
    files[0].id = 11;
    strcpy(files[0].role, "secondary");
    files[1].id = 22;
    strcpy(files[1].role, "primary");
    files[2].id = 33;
    strcpy(files[2].role, "secondary");
    CHECK(book_orbit_catalog_pick_file(files, 3) == 1);
    CHECK(book_orbit_catalog_pick_file(files, 1) == 0);
    CHECK(book_orbit_catalog_pick_file(files, 0) == -1);
    CHECK(book_orbit_catalog_pick_file(NULL, 3) == -1);
}

int main(void) {
    test_api_base();
    test_append_param();
    test_build_filename();
    test_pick_file();
    if (s_failures) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }
    printf("book_orbit_catalog host tests passed\n");
    return 0;
}
