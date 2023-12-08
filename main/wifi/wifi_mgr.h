//
// Created by Hessian on 2023/9/28.
//

#ifndef WT_HOMEGW_WIFI_MGR_H
#define WT_HOMEGW_WIFI_MGR_H

#include <esp_err.h>
#include <wifi_provisioning/manager.h>
#include <freertos/event_groups.h>

void wifi_mgr_init();
void wifi_mgr_start(void);
esp_err_t wifi_mgr_get_ip(char* ip);
void smartconfig_initialise_wifi();
void webconfig_initialise_wifi();
esp_err_t wifi_scan(uint16_t number, wifi_ap_record_t *ap_info, uint16_t *ap_count);

#endif //WT_HOMEGW_WIFI_MGR_H
