#pragma once
#include <cstdint>
constexpr int pdTRUE = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
using BaseType_t = int;
inline uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
