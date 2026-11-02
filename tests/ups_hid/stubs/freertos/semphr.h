#pragma once
#include <cstdint>
struct TestSemaphore {
  bool signaled = false;
};
using SemaphoreHandle_t = TestSemaphore *;
SemaphoreHandle_t xSemaphoreCreateBinary();
void vSemaphoreDelete(SemaphoreHandle_t sem);
void xSemaphoreGive(SemaphoreHandle_t sem);
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks);
