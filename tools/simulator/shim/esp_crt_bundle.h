#pragma once

/* Host shim: the simulator has no TLS stack; attaching a cert bundle always fails. */
typedef struct esp_crt_bundle_s esp_crt_bundle_t;

static inline int esp_crt_bundle_attach(void *conf) {
    (void)conf;
    return -1;
}

static inline void esp_crt_bundle_detach(void *conf) {
    (void)conf;
}
