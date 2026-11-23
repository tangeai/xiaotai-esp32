// SPDX-License-Identifier: MIT
#pragma once

#include "esp_wifi.h"

void wifi_fixture_disconnect_result(esp_err_t result);
void wifi_fixture_connect_result(esp_err_t result);
void wifi_fixture_station_config_result(esp_err_t result);
void wifi_fixture_radio(bool up);
unsigned wifi_fixture_joins(void);
unsigned wifi_fixture_leaves(void);
void wifi_fixture_associated(void);
void wifi_fixture_ipv4(uint32_t ipv4);
void wifi_fixture_physically_disconnected(void);
