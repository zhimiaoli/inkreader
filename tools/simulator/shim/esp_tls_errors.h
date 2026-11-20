#pragma once

/* Host shim: TLS never connects in the simulator; only the retryable-timeout
 * codes referenced by firmware code are needed. */
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x8104
