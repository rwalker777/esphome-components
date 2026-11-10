#pragma once
#include <cstdint>
#include <string>
#include <cstddef>
namespace esphome {
inline uint32_t test_millis = 0;
inline uint32_t millis() { return test_millis; }
inline std::string format_hex_pretty(const uint8_t *, size_t) { return "bytes"; }
}  // namespace esphome
