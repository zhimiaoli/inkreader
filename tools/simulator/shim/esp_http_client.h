#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Host shim: offline esp_http_client. Clients construct, then perform() always
 * fails with a connect error so network features surface as unreachable. */

#ifndef ESP_ERR_HTTP_CONNECT
#define ESP_ERR_HTTP_CONNECT 0x8001
#endif
#ifndef ESP_ERR_HTTP_CONNECTING
#define ESP_ERR_HTTP_CONNECTING 0x8003
#endif
#ifndef ESP_ERR_HTTP_CONNECTION_CLOSED
#define ESP_ERR_HTTP_CONNECTION_CLOSED 0x8004
#endif
#ifndef ESP_ERR_HTTP_FETCH_HEADER
#define ESP_ERR_HTTP_FETCH_HEADER 0x8005
#endif
#ifndef ESP_ERR_HTTP_EAGAIN
#define ESP_ERR_HTTP_EAGAIN 0x8002
#endif
#ifndef ESP_ERR_HTTP_BASE
#define ESP_ERR_HTTP_BASE 0x8000
#endif

typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_OPTIONS
} esp_http_client_method_t;

typedef enum {
    HTTP_ADDR_TYPE_INET = 0,
    HTTP_ADDR_TYPE_INET6,
    HTTP_ADDR_TYPE_ANY
} esp_http_client_addr_type_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADER_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT
} esp_http_client_event_id_t;

typedef struct esp_http_client esp_http_client_t;
typedef struct esp_http_client *esp_http_client_handle_t;

typedef struct {
    const char *url;
    const char *host;
    int port;
    const char *username;
    const char *password;
    const char *path;
    bool use_ssl;
    bool is_async;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
    int max_redirections;
    int max_authorization_retries;
    const char *cert_pem;
    size_t cert_len;
    int (*crt_bundle_attach)(void *conf);
    void *crt_bundle_detach_conf;
    const char *client_cert_pem;
    const char *client_key_pem;
    esp_http_client_addr_type_t addr_type;
    esp_err_t (*event_handler)(struct esp_http_client_event *event);
    void *user_data;
} esp_http_client_config_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    char *data;
    int data_len;
    void *user_data;
    const char *header_key;
    const char *header_value;
    char *output_buffer;
    int output_len;
} esp_http_client_event_t;

struct esp_http_client {
    esp_http_client_config_t cfg;
    int status_code;
    int64_t content_length;
};

static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    static esp_http_client_t client;
    client.cfg = *config;
    client.status_code = 0;
    client.content_length = -1;
    return &client;
}

static inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value) {
    (void)client; (void)key; (void)value;
    return ESP_OK;
}

static inline esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method) {
    (void)client; (void)method;
    return ESP_OK;
}

static inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len) {
    (void)client; (void)data; (void)len;
    return ESP_OK;
}

static inline void esp_http_client_set_user_data(esp_http_client_handle_t client, void *user_data) {
    (void)client; (void)user_data;
}

static inline esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
    (void)client;
    return ESP_ERR_HTTP_CONNECT;
}

static inline int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    return client ? client->status_code : 0;
}

static inline int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) {
    return client ? client->content_length : -1;
}

static inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    (void)client;
    return false;
}

static inline esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t client, int *tls_code, int *verify_flags) {
    (void)client;
    if (tls_code) *tls_code = 0;
    if (verify_flags) *verify_flags = 0;
    return ESP_OK;
}

static inline void esp_http_client_cleanup(esp_http_client_handle_t client) {
    (void)client;
}
