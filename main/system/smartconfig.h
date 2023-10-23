//
// Created by Hessian on 2023/7/29.
//

#ifndef ESP_MENJIN_SMARTCONFIG_H
#define ESP_MENJIN_SMARTCONFIG_H

typedef enum WifiStatus {
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_DISCONNECTED,
} WifiStatus;

typedef void (*SmartConfigCallback)(WifiStatus);

void smartconfig_init(SmartConfigCallback cb);
WifiStatus smartconfig_status();

#endif //ESP_MENJIN_SMARTCONFIG_H
