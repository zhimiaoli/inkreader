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
#include "cJSON.h"
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

static void test_register_payload(void) {
    char payload[224];
    /* md5("abc") = 900150983cd24fb0d6963f7d28e17f72（RFC 1321 向量） */
    assert(book_orbit_register_payload("alice", "abc", payload, sizeof(payload)));
    expect_str(payload, "{\"username\":\"alice\",\"password\":\"900150983cd24fb0d6963f7d28e17f72\"}",
               "register payload");
    /* 空字段与超长用户名拒绝 */
    assert(!book_orbit_register_payload("", "abc", payload, sizeof(payload)));
    assert(!book_orbit_register_payload("alice", "", payload, sizeof(payload)));
    char longname[65];
    memset(longname, 'a', 64);
    longname[64] = 0;
    assert(!book_orbit_register_payload(longname, "abc", payload, sizeof(payload)));
    /* JSON 特殊字符拒绝 */
    assert(!book_orbit_register_payload("al\"ice", "abc", payload, sizeof(payload)));
    assert(!book_orbit_register_payload("al\\ice", "abc", payload, sizeof(payload)));
    assert(!book_orbit_register_payload(NULL, "abc", payload, sizeof(payload)));
}

static void test_register_accepted(void) {
    assert(book_orbit_register_accepted(201));
    assert(book_orbit_register_accepted(200));
    assert(!book_orbit_register_accepted(402));
    assert(!book_orbit_register_accepted(401));
    assert(!book_orbit_register_accepted(500));
    assert(!book_orbit_register_accepted(0));
}

static void test_highlight_pos(void) {
    char buf[64];
    assert(book_orbit_highlight_pos(0u, buf, sizeof(buf)));
    expect_str(buf, "/body/TextMemo[0]", "pos0 zero");
    assert(book_orbit_highlight_pos(4294967295u, buf, sizeof(buf)));
    expect_str(buf, "/body/TextMemo[4294967295]", "pos0 max");
    /* 缓冲不足拒绝（"/body/TextMemo[" 已 16 字符 + 数字 + ']' + NUL）。 */
    assert(!book_orbit_highlight_pos(0u, buf, 16));
    assert(!book_orbit_highlight_pos(0u, NULL, sizeof(buf)));
}

static void test_push_key(void) {
    char k[33], dt[20];
    /* 键 = md5(datetime | pos0)，与 book_orbit_annotation_key 的组合一致性。 */
    assert(book_orbit_push_key(1794873600u, 12345u, k, dt));
    expect_str(dt, "2026-11-17 00:00:00", "push key dt");
    char pos[64];
    assert(book_orbit_highlight_pos(12345u, pos, sizeof(pos)));
    char want[33];
    assert(book_orbit_annotation_key(dt, pos, want));
    expect_str(k, want, "push key composition");
    /* 时间戳 0 也能成键；同锚点不同时间 → 不同键。 */
    char k2[33], dt2[20];
    assert(book_orbit_push_key(1794873601u, 12345u, k2, dt2));
    assert(strcmp(k, k2) != 0);
    assert(book_orbit_push_key(1794873600u, 12346u, k2, dt2));
    assert(strcmp(k, k2) != 0);
}

static void test_exchange_body(void) {
    book_orbit_push_entry_t changes[2] = {0};
    changes[0].timestamp = 1794873600u;
    changes[0].start_offset = 100u;
    changes[0].end_offset = 250u;
    changes[0].page = 3;
    changes[0].text = "第一段划线";
    changes[1].timestamp = 1794873601u;
    changes[1].start_offset = 4096u;
    changes[1].end_offset = 4200u;
    changes[1].page = 0; /* pageno <=0 不序列化 */
    changes[1].text = "with \"quote\" and \\ backslash";
    book_orbit_push_key_t keys[2];
    assert(book_orbit_push_key(1794873600u, 100u, keys[0].k, keys[0].dt));
    assert(book_orbit_push_key(1794873601u, 4096u, keys[1].k, keys[1].dt));
    char *body = NULL;
    assert(book_orbit_annotations_exchange_body("kiikoread-aabbccddeeff", "S31", "deadbeef",
                                                changes, 2, keys, 2, true, &body));
    cJSON *root = cJSON_Parse(body);
    assert(root);
    expect_str(cJSON_GetObjectItem(root, "deviceId")->valuestring, "kiikoread-aabbccddeeff",
               "deviceId");
    expect_str(cJSON_GetObjectItem(root, "pluginVersion")->valuestring, "kiikoread-bo-1",
               "pluginVersion");
    cJSON *book = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "books"), 0);
    assert(book);
    expect_str(cJSON_GetObjectItem(book, "hash")->valuestring, "deadbeef", "hash");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(book, "keysComplete")));
    cJSON *change_array = cJSON_GetObjectItem(book, "changes");
    assert(cJSON_GetArraySize(change_array) == 2);
    cJSON *c0 = cJSON_GetArrayItem(change_array, 0);
    expect_str(cJSON_GetObjectItem(c0, "datetime")->valuestring, "2026-11-17 00:00:00", "c0 dt");
    expect_str(cJSON_GetObjectItem(c0, "drawer")->valuestring, "lighten", "c0 drawer");
    expect_str(cJSON_GetObjectItem(c0, "posFormat")->valuestring, "xpointer", "c0 posFormat");
    expect_str(cJSON_GetObjectItem(c0, "pos0")->valuestring, "/body/TextMemo[100]", "c0 pos0");
    expect_str(cJSON_GetObjectItem(c0, "pos1")->valuestring, "/body/TextMemo[250]", "c0 pos1");
    expect_str(cJSON_GetObjectItem(c0, "text")->valuestring, "第一段划线", "c0 text");
    assert(cJSON_IsNumber(cJSON_GetObjectItem(c0, "pageno")) &&
           cJSON_GetObjectItem(c0, "pageno")->valuedouble == 3);
    cJSON *c1 = cJSON_GetArrayItem(change_array, 1);
    expect_str(cJSON_GetObjectItem(c1, "pos0")->valuestring, "/body/TextMemo[4096]", "c1 pos0");
    assert(cJSON_GetObjectItem(c1, "pageno") == NULL);
    expect_str(cJSON_GetObjectItem(c1, "text")->valuestring, "with \"quote\" and \\ backslash",
               "c1 escaping");
    cJSON *key_array = cJSON_GetObjectItem(book, "keys");
    assert(cJSON_GetArraySize(key_array) == 2);
    cJSON *row = cJSON_GetArrayItem(key_array, 0);
    expect_str(cJSON_GetObjectItem(row, "k")->valuestring, keys[0].k, "row k");
    expect_str(cJSON_GetObjectItem(row, "dt")->valuestring, keys[0].dt, "row dt");
    cJSON_Delete(root);
    free(body);

    /* 空键集 + keysComplete=false 也要合法（无删除处理的拉取形态）。 */
    body = NULL;
    assert(book_orbit_annotations_exchange_body("dev", "model", "h", changes, 1, NULL, 0, false,
                                                &body));
    root = cJSON_Parse(body);
    assert(root && cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(
                                                        cJSON_GetObjectItem(root, "books"), 0),
                                                    "keysComplete")));
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "books"), 0), "keys") ==
           NULL);
    cJSON_Delete(root);
    free(body);

    /* 拒绝路径：超过批量、缺文本、空指针。 */
    book_orbit_push_entry_t overflow[BOOK_ORBIT_PUSH_BATCH + 1] = {0};
    for (size_t i = 0; i <= BOOK_ORBIT_PUSH_BATCH; ++i) overflow[i].text = "x";
    assert(!book_orbit_annotations_exchange_body("dev", "model", "h", overflow,
                                                 BOOK_ORBIT_PUSH_BATCH + 1, NULL, 0, true, &body));
    assert(!book_orbit_annotations_exchange_body("dev", "model", "h", changes, 2, NULL, 2, true,
                                                 &body));
    book_orbit_push_entry_t notext = changes[0];
    notext.text = NULL;
    assert(!book_orbit_annotations_exchange_body("dev", "model", "h", &notext, 1, NULL, 0, true,
                                                 &body));
    assert(!book_orbit_annotations_exchange_body(NULL, "model", "h", changes, 1, NULL, 0, true,
                                                 &body));
    assert(!book_orbit_annotations_exchange_body("dev", "model", "h", changes, 1, NULL, 0, true,
                                                 NULL));
}

int main(void) {
    test_annotation_key();
    test_datetime();
    test_normalize();
    test_document_hash();
    test_register_payload();
    test_register_accepted();
    test_highlight_pos();
    test_push_key();
    test_exchange_body();
    puts("book_orbit host tests passed");
    return 0;
}
