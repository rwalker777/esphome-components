#pragma once
inline const char *esp_err_to_name(int) { return "test error"; }
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_SIZE = 2;
constexpr int ESP_ERR_INVALID_STATE = 3, ESP_ERR_NO_MEM = 4, ESP_ERR_TIMEOUT = 5;
constexpr int ESP_ERR_NOT_SUPPORTED = 6;
constexpr int ESP_ERR_NOT_FOUND = 7, ESP_ERR_INVALID_RESPONSE = 8;
