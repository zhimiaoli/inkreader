/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：book_opds 纯函数宿主测试：相对链接解析、搜索模板展开、格式名、Basic 认证与
 *      OPDS 2.0 目录 JSON 解析（含分页链接、导航、出版物 acquisition 挑选与 Atom 拒绝）。
 * English: Host tests for book_opds pure helpers: relative href resolution, search
 *      template expansion, format names, Basic auth and OPDS 2.0 catalog JSON parsing
 *      (pagination links, navigation, acquisition picking, Atom rejection).
 */
#include <stdio.h>
#include <string.h>

#include "book_opds.h"

static int s_failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                 \
        }                                                                 \
    } while (0)

static void expect_str(const char* got, const char* want, const char* what) {
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
        s_failures++;
    }
}

static void test_resolve_href(void) {
    char out[256];
    /* 绝对地址原样、fragment 剥掉。Absolute verbatim, fragment dropped. */
    CHECK(book_opds_resolve_href("https://a/b/c.json", "https://x/y.json?t=1#frag", out, sizeof(out)));
    expect_str(out, "https://x/y.json?t=1", "absolute");
    /* //host、/path、相对路径。 //host, /path, relative. */
    CHECK(book_opds_resolve_href("https://a/b/c.json", "//h/n.json", out, sizeof(out)));
    expect_str(out, "https://h/n.json", "scheme-relative");
    CHECK(book_opds_resolve_href("https://a/b/c.json", "/root.json", out, sizeof(out)));
    expect_str(out, "https://a/root.json", "path-absolute");
    CHECK(book_opds_resolve_href("https://a/b/c.json", "page2.json", out, sizeof(out)));
    expect_str(out, "https://a/b/page2.json", "relative");
    CHECK(book_opds_resolve_href("https://a/b/c.json?page=1", "page2.json", out, sizeof(out)));
    expect_str(out, "https://a/b/page2.json", "relative over query");
    CHECK(book_opds_resolve_href("https://a/b/c.json", "sub/dir/d.json", out, sizeof(out)));
    expect_str(out, "https://a/b/sub/dir/d.json", "nested relative");
    /* 失败分支：无 base、无 scheme、仅 query、超长。Failure branches. */
    CHECK(!book_opds_resolve_href("", "x", out, sizeof(out)));
    CHECK(!book_opds_resolve_href("notaurl", "x", out, sizeof(out)));
    CHECK(!book_opds_resolve_href("https://a/b", "?only=query", out, sizeof(out)));
    CHECK(!book_opds_resolve_href("https://a/b/c.json", "", out, sizeof(out)));
    char tiny[8];
    CHECK(!book_opds_resolve_href("https://a/b", "page2.json", tiny, sizeof(tiny)));
}

static void test_expand_search(void) {
    char out[256];
    /* {query} 与 {searchTerms} 直接替换并编码。{query} and {searchTerms} encoded. */
    CHECK(book_opds_expand_search("https://s/search?q={query}", "a b&c", out, sizeof(out)));
    expect_str(out, "https://s/search?q=a%20b%26c", "query template");
    CHECK(book_opds_expand_search("https://s/search/{searchTerms}", "三体", out, sizeof(out)));
    expect_str(out, "https://s/search/%E4%B8%89%E4%BD%93", "searchTerms template");
    /* {?var} 与 {?var=default}。{?var} and {?var=default}. */
    CHECK(book_opds_expand_search("https://s/search{?query}", "abc", out, sizeof(out)));
    expect_str(out, "https://s/search?query=abc", "query-expansion");
    CHECK(book_opds_expand_search("https://s/search{?q=books}", "abc", out, sizeof(out)));
    expect_str(out, "https://s/search?q=abc", "default-var expansion");
    /* 纯 URL 无模板 → 追加；已有 ? → &。Plain URL appends; existing ? uses &. */
    CHECK(book_opds_expand_search("https://s/search", "x y", out, sizeof(out)));
    expect_str(out, "https://s/search?query=x%20y", "plain append");
    CHECK(book_opds_expand_search("https://s/search?page=1", "x", out, sizeof(out)));
    expect_str(out, "https://s/search?page=1&query=x", "plain append with query");
    /* 未知表达式拒绝；空 query 拒绝。Unknown expressions and empty queries are rejected. */
    CHECK(!book_opds_expand_search("https://s/{unknown}", "x", out, sizeof(out)));
    CHECK(!book_opds_expand_search("https://s/{query}", "", out, sizeof(out)));
    CHECK(!book_opds_expand_search(NULL, "", out, sizeof(out)));
}

static void test_format_name(void) {
    char out[24];
    CHECK(book_opds_format_name("application/epub+zip", out, sizeof(out)));
    expect_str(out, "EPUB", "epub");
    CHECK(book_opds_format_name("application/pdf", out, sizeof(out)));
    expect_str(out, "PDF", "pdf");
    CHECK(book_opds_format_name("text/plain", out, sizeof(out)));
    expect_str(out, "TXT", "txt");
    CHECK(book_opds_format_name("application/vnd.unknown+zip", out, sizeof(out)));
    expect_str(out, "VND.UNKNOWN", "unknown subtype");
    CHECK(book_opds_format_name("", out, sizeof(out)));
    expect_str(out, "BOOK", "empty");
    CHECK(book_opds_format_name(NULL, out, sizeof(out)));
    expect_str(out, "BOOK", "null");
}

static void test_basic_auth(void) {
    char out[64];
    CHECK(book_opds_basic_auth("user", "pass", out, sizeof(out)));
    expect_str(out, "dXNlcjpwYXNz", "basic user:pass");
    CHECK(book_opds_basic_auth("a", "b:c", out, sizeof(out)));
    expect_str(out, "YTpiOmM=", "basic padded");
    CHECK(book_opds_basic_auth("", "", out, sizeof(out)));
    expect_str(out, "Og==", "basic empty");
    char small[8];
    CHECK(!book_opds_basic_auth("user", "pass", small, sizeof(small)));
}

static void test_parse_catalog(void) {
    const char* catalog =
        "{"
        "\"metadata\":{\"title\":\"示例书库\"},"
        "\"links\":["
        "{\"rel\":\"self\",\"href\":\"https://opds.example/root.json\"},"
        "{\"rel\":\"next\",\"href\":\"page2.json\"},"
        "{\"rel\":\"prev\",\"href\":\"https://opds.example/page0.json\"},"
        "{\"rel\":\"search\",\"href\":\"https://opds.example/search{?query}\"}"
        "],"
        "\"navigation\":["
        "{\"href\":\"/nav/latest.json\",\"title\":\"最新\"},"
        "{\"href\":\"nav/byauthor.json\",\"title\":\"按作者\"}"
        "],"
        "\"publications\":["
        "{"
        "\"metadata\":{\"title\":\"三体\",\"author\":[{\"name\":\"刘慈欣\"},{\"name\":\"其他人\"}]},"
        "\"links\":["
        "{\"rel\":\"http://opds-spec.org/image\",\"href\":\"cover.jpg\",\"type\":\"image/jpeg\"},"
        "{\"rel\":\"http://opds-spec.org/acquisition\",\"href\":\"/files/santi.pdf\",\"type\":\"application/pdf\"},"
        "{\"rel\":\"http://opds-spec.org/acquisition\",\"href\":\"/files/santi.epub\",\"type\":\"application/epub+zip\"}"
        "]"
        "},"
        "{"
        "\"metadata\":{\"title\":\"无直链书\",\"author\":[{\"name\":\"作者\"}]},"
        "\"links\":[{\"rel\":\"http://opds-spec.org/image\",\"href\":\"c.jpg\",\"type\":\"image/jpeg\"}]"
        "}"
        "],"
        "\"groups\":["
        "{\"metadata\":{\"title\":\"分组\"},\"links\":[{\"rel\":\"self\",\"href\":\"group.json\"}]}"
        "]"
        "}";
    char title[128];
    char next[224];
    char prev[224];
    char search[224];
    char self[224];
    book_opds_entry_t entries[BOOK_OPDS_ENTRIES_MAX];
    int count = 0;
    CHECK(book_opds_parse_catalog(catalog, "https://opds.example/root.json", title, sizeof(title),
                                  entries, BOOK_OPDS_ENTRIES_MAX, &count, next, prev, search, self,
                                  sizeof(self)));
    expect_str(title, "示例书库", "catalog title");
    expect_str(self, "https://opds.example/root.json", "self");
    expect_str(next, "https://opds.example/page2.json", "next resolved");
    expect_str(prev, "https://opds.example/page0.json", "prev");
    expect_str(search, "https://opds.example/search{?query}", "search template kept");
    /* 条目顺序：导航 2 + 出版物 1（无直链的被剔除）+ 组 1 = 4。 */
    /* Order: 2 navigation + 1 publication (no-acquisition dropped) + 1 group = 4. */
    CHECK(count == 4);
    CHECK(!entries[0].publication);
    expect_str(entries[0].title, "最新", "nav title");
    expect_str(entries[0].href, "https://opds.example/nav/latest.json", "nav href");
    expect_str(entries[1].href, "https://opds.example/nav/byauthor.json",
               "relative nav against self directory");
    CHECK(entries[2].publication);
    expect_str(entries[2].title, "三体", "pub title");
    expect_str(entries[2].authors, "刘慈欣, 其他人", "pub authors");
    expect_str(entries[2].href, "https://opds.example/files/santi.epub", "epub preferred");
    expect_str(entries[2].media_type, "application/epub+zip", "media type");
    expect_str(entries[2].format, "EPUB", "format");
    CHECK(!entries[3].publication);
    expect_str(entries[3].title, "分组", "group title");
    expect_str(entries[3].href, "https://opds.example/group.json", "group href");
    /* Atom（OPDS 1.x）拒绝。Atom (OPDS 1.x) is rejected. */
    const char* atom = "{\"<feed\":{\"id\":\"x\"}}";
    count = -1;
    CHECK(!book_opds_parse_catalog(atom, "https://opds.example/root.json", title, sizeof(title),
                                   entries, BOOK_OPDS_ENTRIES_MAX, &count, next, prev, search, self,
                                   sizeof(self)));
    /* 空目录合法但 0 条。An empty catalog parses with zero entries. */
    const char* empty = "{\"metadata\":{\"title\":\"空\"},\"navigation\":[]}";
    CHECK(book_opds_parse_catalog(empty, "https://opds.example/root.json", title, sizeof(title),
                                  entries, BOOK_OPDS_ENTRIES_MAX, &count, next, prev, search, self,
                                  sizeof(self)));
    CHECK(count == 0);
    /* 无 self 链接时相对分页用请求 URL 为基准。No self: pagination resolves against the
       request URL. */
    const char* no_self =
        "{\"metadata\":{\"title\":\"t\"},"
        "\"links\":[{\"rel\":\"next\",\"href\":\"p2.json\"}],"
        "\"publications\":[{\"metadata\":{\"title\":\"b\"},"
        "\"links\":[{\"rel\":\"http://opds-spec.org/acquisition\",\"href\":\"b.epub\","
        "\"type\":\"application/epub+zip\"}]}]}";
    CHECK(book_opds_parse_catalog(no_self, "https://opds.example/v2/root.json", title, sizeof(title),
                                  entries, BOOK_OPDS_ENTRIES_MAX, &count, next, prev, search, self,
                                  sizeof(self)));
    expect_str(next, "https://opds.example/v2/p2.json", "next against request url");
    CHECK(count == 1);
    expect_str(entries[0].href, "https://opds.example/v2/b.epub", "acquisition against request url");
}

int main(void) {
    test_resolve_href();
    test_expand_search();
    test_format_name();
    test_basic_auth();
    test_parse_catalog();
    if (s_failures) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }
    printf("book_opds host tests passed\n");
    return 0;
}
