#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Host shim: the CX25601N charger is not present in the simulator. */
#define METALIO_CHARGER_STD_MA  1000
#define METALIO_CHARGER_FAST_MA 1500

esp_err_t metalio_charger_init(void);
bool metalio_charger_ready(void);
esp_err_t metalio_charger_set_current_ma(uint32_t ma);
uint32_t metalio_charger_current_ma(void);
esp_err_t metalio_charger_set_fast_charge(bool on);
bool metalio_charger_fast_charge(void);
void metalio_charger_suspend(void);
void metalio_charger_resume(void);
