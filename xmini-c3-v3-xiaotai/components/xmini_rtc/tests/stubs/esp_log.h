#pragma once
void test_log(const char *level, const char *tag, const char *format, ...);
#define ESP_LOGI(tag, ...) test_log("I", tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log("W", tag, __VA_ARGS__)
