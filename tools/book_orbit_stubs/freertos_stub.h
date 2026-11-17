#pragma once
#include <stdint.h>
#include <stdbool.h>
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
typedef void *TaskHandle_t;
void vTaskDelay(uint32_t ticks);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t task);
