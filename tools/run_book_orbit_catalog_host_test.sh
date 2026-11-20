#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 在仓库根目录运行；产物仅在 build。/ Run from the repository root; artifacts stay in build.
set -euo pipefail
mkdir -p build/book-tests build/book-orbit-src
# 同目录引号包含会优先命中 main/book 下的真实头文件；把被测源复制到独立目录让替身生效。
# Same-directory quoted includes win over -I; copy the sources out so the stubs shadow the real headers.
cp main/book/book_orbit_sync.c build/book-orbit-src/book_orbit_sync.c
cp main/book/book_orbit_sync.h build/book-orbit-src/book_orbit_sync.h
cp main/book/book_orbit_catalog.c build/book-orbit-src/book_orbit_catalog.c
cp main/book/book_orbit_catalog.h build/book-orbit-src/book_orbit_catalog.h
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer
       -ffunction-sections -fdata-sections
       -DBOOK_ORBIT_STATE_DIR='"build/book-orbit-test-state"'
       -Itools/book_orbit_stubs -Itools/book_orbit_catalog_stubs -Imain/book -Ibuild/book-orbit-src)
gcc "${flags[@]}" -Wl,--gc-sections tools/book_orbit_catalog_host_test.c \
    build/book-orbit-src/book_orbit_sync.c build/book-orbit-src/book_orbit_catalog.c \
    -o build/book-tests/book_orbit_catalog
build/book-tests/book_orbit_catalog
