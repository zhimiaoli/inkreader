#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Host shim for main/book/metalio_jpeg.c: the S31 hardware JPEG engine does
 * not exist here, so the decode API reports unsupported and callers fall back
 * to the libjpeg-turbo software path. */

typedef enum { JPEG_DEC_ALLOC_INPUT_BUFFER = 0, JPEG_DEC_ALLOC_OUTPUT_BUFFER } jpeg_dec_buffer_alloc_type_t;
typedef enum { JPEG_DECODE_OUT_FORMAT_RGB888 = 0, JPEG_DECODE_OUT_FORMAT_GRAY } jpeg_decode_output_format_t;
#define JPEG_DEC_RGB_ELEMENT_ORDER_RGB 0
#define JPEG_YUV_RGB_CONV_STD_BT601 0

typedef struct jpeg_decoder_impl *jpeg_decoder_handle_t;

typedef struct {
    jpeg_dec_buffer_alloc_type_t buffer_direction;
} jpeg_decode_memory_alloc_cfg_t;

typedef struct {
    uint32_t timeout_ms;
} jpeg_decode_engine_cfg_t;

typedef struct {
    jpeg_decode_output_format_t output_format;
    int rgb_order;
    int conv_std;
} jpeg_decode_cfg_t;

static inline uint8_t *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *actual_size) {
    (void)size; (void)cfg;
    if (actual_size) *actual_size = 0;
    return NULL;
}

static inline esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *engine_cfg, jpeg_decoder_handle_t *ret_handle) {
    (void)engine_cfg;
    if (ret_handle) *ret_handle = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t handle, const jpeg_decode_cfg_t *decode_cfg,
                                             const uint8_t *input_buffer, size_t input_len,
                                             uint8_t *output_buffer, size_t output_len, size_t *output_size) {
    (void)handle; (void)decode_cfg; (void)input_buffer; (void)input_len;
    (void)output_buffer; (void)output_len;
    if (output_size) *output_size = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t handle) {
    (void)handle;
    return ESP_OK;
}
