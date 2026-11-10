#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void *;
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t, void *arg, unsigned, TaskHandle_t *handle);
void vTaskDelay(uint32_t ticks);
void vTaskDelete(TaskHandle_t);
