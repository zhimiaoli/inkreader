#pragma once
#include "cJSON.h"

typedef enum { HTTP_EVENT_ON_DATA = 1 } esp_http_client_event_id_t;
typedef enum { HTTP_METHOD_GET = 0, HTTP_METHOD_POST, HTTP_METHOD_PUT } esp_http_client_method_t;
typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    char *data;
    int data_len;
    void *user_data;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    esp_err_t (*event_handler)(esp_http_client_event_t *event);
    void (*crt_bundle_attach)(void *conf);
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *user_data);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
