/* SPDX-License-Identifier: Apache-2.0
 * 中文：BookOrbit 纯函数宿主测试：MD5 身份键/文档部分哈希、日期时间往返、服务器地址归一化。
 * 测试向量由 Python hashlib 生成（见文内注释）。
 * English: Host tests for BookOrbit pure helpers: MD5 identity key / partial document hash,
 * datetime round trips and server URL normalization. Vectors generated with Python hashlib.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "book_orbit_sync.h"

static void expect_str(const char *got, const char *want, const char *what) {
    if (strcmp(got, want)) {
        fprintf(stderr, "%s mismatch\n  want=%s\n  got =%s\n", what, want, got);
        abort();
    }
}

static void test_annotation_key(void) {
    char key[33];
    /* md5("abc") — RFC 1321 向量 */
    assert(book_orbit_annotation_key("a", "bc", key));
    expect_str(key, "900150983cd24fb0d6963f7d28e17f72", "key(abc)");
    /* 200 字符输入：跨块填充路径（used>56 的二次分块） */
    char datetime[201];
    for (int i = 0; i < 200; ++i) datetime[i] = (char)('a' + (i % 26));
    datetime[200] = 0;
    assert(book_orbit_annotation_key(datetime, "zz", key));
    expect_str(key, "b40f36c317a91e56db2827320a9f0727", "key(long200+zz)");
    assert(!book_orbit_annotation_key("2026-01-01 00:00:00", "", key));
    assert(!book_orbit_annotation_key("", "x", key));
    assert(!book_orbit_annotation_key(NULL, "x", key));
}

static void test_datetime(void) {
    char text[20];
    assert(book_orbit_format_datetime(0u, text));
    expect_str(text, "1970-01-01 00:00:00", "epoch0");
    /* 2026-11-17 00:00:00 UTC */
    assert(book_orbit_format_datetime(1794873600u, text));
    expect_str(text, "2026-11-17 00:00:00", "sample");
    assert(book_orbit_format_datetime(951782400u, text));
    expect_str(text, "2000-02-29 00:00:00", "leap");
    uint32_t epoch = 0;
    assert(book_orbit_parse_datetime("2000-02-29 12:34:56", &epoch) && epoch == 951827696u);
    assert(book_orbit_parse_datetime(text, &epoch) && epoch == 951782400u);
    /* 往返一致 */
    for (uint32_t probe = 1700000000u; probe < 1700010000u; probe += 999) {
        char buf[20];
        assert(book_orbit_format_datetime(probe, buf));
        assert(book_orbit_parse_datetime(buf, &epoch) && epoch == probe);
    }
    assert(!book_orbit_parse_datetime("2026-1-1 0:0:0", &epoch));
    assert(!book_orbit_parse_datetime("2026-01-01T00:00:00", &epoch));
    assert(!book_orbit_parse_datetime("2026-13-01 00:00:00", &epoch));
    assert(!book_orbit_parse_datetime("2026-02-30 00:00:00", &epoch));
    assert(!book_orbit_parse_datetime("2026-01-01 24:00:00", &epoch));
    assert(!book_orbit_parse_datetime("1969-12-31 23:59:59", &epoch));
}

static void test_normalize(void) {
    char out[224];
    assert(book_orbit_normalize_server_url("bookorbit.example.com", out, sizeof(out)));
    expect_str(out, "https://bookorbit.example.com", "bare host");
    assert(book_orbit_normalize_server_url("http://10.0.0.5:7333", out, sizeof(out)));
    expect_str(out, "http://10.0.0.5:7333", "explicit http");
    assert(book_orbit_normalize_server_url("https://bo.example.com/api/v1/koreader/", out, sizeof(out)));
    expect_str(out, "https://bo.example.com", "pasted full API path");
    assert(book_orbit_normalize_server_url("  bo.example.com/api/v1  ", out, sizeof(out)));
    expect_str(out, "https://bo.example.com", "trim and strip v1");
    assert(!book_orbit_normalize_server_url("", out, sizeof(out)));
    assert(!book_orbit_normalize_server_url("   ", out, sizeof(out)));
}

static void test_document_hash(void) {
    const char *path = "build/book-orbit-test-sample.bin";
    FILE *file = fopen(path, "wb");
    assert(file);
    for (int i = 0; i < 5000; ++i) fputc((i * 7) % 256, file);
    fclose(file);
    char hash[33];
    assert(book_orbit_document_hash(path, hash));
    /* Python: md5(update(data[o:o+1024]) for o in (0,1024,4096)) — 5000 字节样本 */
    expect_str(hash, "41be506b8937f8d7bad94af59c7d589a", "partial hash 5000B");
    file = fopen(path, "wb");
    for (int i = 0; i < 500; ++i) fputc((i * 7) % 256, file);
    fclose(file);
    assert(book_orbit_document_hash(path, hash));
    expect_str(hash, "e730b68a00da6a1eadb2c524fc5f0f2b", "partial hash 500B");
    assert(!book_orbit_document_hash("build/does-not-exist.bin", hash));
}

int main(void) {
    test_annotation_key();
    test_datetime();
    test_normalize();
    test_document_hash();
    puts("book_orbit host tests passed");
    return 0;
}
