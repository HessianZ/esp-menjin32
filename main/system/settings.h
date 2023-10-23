//
// Created by Hessian on 2023/7/29.
//

#pragma once
#ifndef ESP_MENJIN_SETTINGS_H
#define ESP_MENJIN_SETTINGS_H

#include "esp_err.h"

typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_url[64];
    char mqtt_username[32];
    char mqtt_password[64];
    uint32_t i2c_clock;
    uint8_t i2c_address;
} sys_param_t;

esp_err_t settings_read_parameter_from_nvs(void);
esp_err_t settings_write_parameter_to_nvs(void);
sys_param_t *settings_get_parameter(void);
sys_param_t settings_get_default_parameter(void);
void settings_dump(void);

#endif