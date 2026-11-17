#pragma once
#include "cJSON.h"
typedef enum { READ_PICO_TRANSFER_STOPPED = 0, READ_PICO_TRANSFER_IDLE, READ_PICO_TRANSFER_ACTIVE } read_pico_transfer_state_t;
typedef struct {
    read_pico_transfer_state_t state;
    bool network_ready;
} read_pico_transfer_status_t;
typedef enum { READ_PICO_TRANSFER_MODE_STA = 0 } read_pico_transfer_mode_t;
typedef struct {
    read_pico_transfer_mode_t mode;
    bool network_only;
} read_pico_transfer_cfg_t;
void read_pico_transfer_get_status(read_pico_transfer_status_t *status);
esp_err_t read_pico_transfer_get_saved_wifi(char *ssid, bool *configured);
esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg);
esp_err_t read_pico_transfer_stop(void);
void read_pico_transfer_service_poll(void);
esp_err_t read_pico_transfer_sync_time_online(uint32_t *utc_seconds);
