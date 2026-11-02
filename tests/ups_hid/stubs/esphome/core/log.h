#pragma once
#include <cstdio>
#define ESP_LOGE(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGW(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGI(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGD(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGV(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGVV(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) std::fprintf(stderr, __VA_ARGS__)
