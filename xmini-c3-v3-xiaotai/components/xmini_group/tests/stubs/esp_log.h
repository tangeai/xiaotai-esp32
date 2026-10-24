#pragma once
void group_test_log(const char *tag, const char *format, ...);
#define ESP_LOGW(...) group_test_log(__VA_ARGS__)
