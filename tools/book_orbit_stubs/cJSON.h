/* 宿主测试替身：仅声明 book_orbit_sync.c 编译所需的符号，未引用的函数会被 gc-sections 裁掉。
 * Host-test stubs: declarations only; unreferenced functions are discarded via gc-sections. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

/* cJSON — 仅声明 */
typedef struct cJSON {
    struct cJSON *next, *child;
    int type;
    char *valuestring;
    double valuedouble;
} cJSON;
cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
bool cJSON_IsString(const cJSON *item);
bool cJSON_IsNumber(const cJSON *item);
bool cJSON_IsBool(const cJSON *item);
bool cJSON_IsTrue(const cJSON *item);
bool cJSON_IsObject(const cJSON *item);
bool cJSON_IsArray(const cJSON *item);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *string, const char *valuestring);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *string, double num);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *string, bool value);
cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name);
void cJSON_AddItemToArray(cJSON *array, cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);
