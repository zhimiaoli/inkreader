#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Host shim: S31 metalio_hw has no I2C expander, TPS rails, haptics or
 * frontlight PWM in the simulator. These stubs keep the UI paths alive with
 * neutral hardware state; the reader quick-menu tracks its own light state. */

esp_err_t metalio_hw_init(void);
esp_err_t metalio_tps_prepare(void);
void metalio_tps_cancel_prepare(void);
bool metalio_key_down(void);
bool metalio_confirm_power_key(void);
bool metalio_confirm_light_wake_key(void);
bool metalio_boot_power_key(void);
void metalio_capture_boot_power_key(void);
int metalio_take_volume(void);
int metalio_take_ai(void);
void metalio_ai_clear(void);
void metalio_volume_clear(void);
void metalio_ai_sample(bool down, int64_t now_ms);
void metalio_volume_sample(uint16_t inputs, int64_t now_ms);
enum { METALIO_AI_NONE, METALIO_AI_SHORT, METALIO_AI_LONG };

bool metalio_frontlight_enabled(void);
unsigned metalio_frontlight_position(bool warmth);
void metalio_frontlight_preview_position(unsigned brightness, unsigned warmth);
void metalio_frontlight_preview(unsigned brightness, unsigned warmth);
void metalio_frontlight_toggle(void);
void metalio_frontlight_commit(void);
void metalio_frontlight_save(unsigned brightness, unsigned warmth);
void metalio_frontlight_restore(void);
void metalio_frontlight(unsigned brightness, unsigned warmth);
unsigned metalio_brightness(void);
unsigned metalio_warmth(void);
#define METALIO_FRONTLIGHT_RANGE 240

bool metalio_haptic_enabled(void);
void metalio_haptic_set_enabled(bool enabled);
esp_err_t metalio_haptic_click(void);
void metalio_haptic_stop(void);
void metalio_haptic_resume(void);

bool metalio_download_connected(void);
void metalio_clear_download_request(void);
bool metalio_usb_blocks_sleep(void);
void metalio_usb_describe(char *out, size_t size);
void metalio_poweroff(void);
void metalio_hold_for_deep_sleep(void);
void metalio_sd_release_hold(void);
