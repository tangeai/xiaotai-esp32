#pragma once
#define ESP_IDF_VERSION_VAL(a,b,c) (((a) << 16) | ((b) << 8) | (c))
#if defined(UI_TEST_IDF_504)
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 4, 0)
#elif defined(UI_TEST_IDF_555)
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5, 5, 5)
#elif defined(UI_TEST_IDF_602)
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6, 0, 2)
#else
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6, 0, 3)
#endif
