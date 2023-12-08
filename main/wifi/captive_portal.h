//
// Created by Hessian on 2023/9/6.
//

#ifndef ESP_FOLLOWME2_CAPTIVE_PORTAL_H
#define ESP_FOLLOWME2_CAPTIVE_PORTAL_H

#include "esp_http_server.h"

esp_err_t start_captive_portal(const char* base_path);
esp_err_t stop_captive_portal();

#endif //ESP_FOLLOWME2_CAPTIVE_PORTAL_H
