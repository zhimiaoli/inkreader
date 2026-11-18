#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#define BOOK_STORE_PATH_MAX 288
#define BOOK_STORE_ROOT_MAX 3

typedef struct {
    char path[128];
    bool is_flash;
} book_store_root_t;

esp_err_t book_store_upload_root(book_store_root_t* out);
void book_store_notify_changed(void);
unsigned book_store_revision(void);
