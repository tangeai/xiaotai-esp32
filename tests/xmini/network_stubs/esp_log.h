// SPDX-License-Identifier: MIT
#pragma once
void test_network_log(char level, const char *tag, const char *format, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(...) test_network_log('I', __VA_ARGS__)
#define ESP_LOGW(...) test_network_log('W', __VA_ARGS__)
#define ESP_LOGE(...) test_network_log('E', __VA_ARGS__)
