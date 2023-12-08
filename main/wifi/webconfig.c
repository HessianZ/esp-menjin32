//
// Created by Hessian on 2023/11/10.
//

#include <string.h>
#include <stdlib.h>
#include <lwip/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "wifi_mgr.h"
#include "esp_mac.h"
#include "captive_portal.h"
#include "dns_server.h"
#include "cJSON.h"

static const char *TAG = "webconfig";
static const int WIFI_CONNECTED_EVENT = BIT0;

esp_netif_t* netif;
static bool provisioned = false;
static EventGroupHandle_t s_wifi_event_group;
static dns_server_handle_t dns_server;

/* Event handler for catching system events */
static void webconfig_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        ESP_LOGD(TAG, "Event --- WIFI_EVENT -- %ld", event_id);

        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "WIFI_EVENT_AP_START");
        } else if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGD(TAG, "Event --- WIFI_EVENT_STA_START");

            ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

            if (provisioned) {
                ESP_ERROR_CHECK(esp_wifi_connect());
            }

        } else if (event_id == WIFI_EVENT_WIFI_READY) {
            ESP_LOGD(TAG, "Event --- WIFI_EVENT_WIFI_READY");
            ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");

            ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

            if (provisioned) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
            }
        }
    } else if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                              "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                              "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                esp_wifi_disconnect();
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "Provisioning end");
                // esp_bt_controller_deinit();
                // esp_bt_controller_disable();
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            case WIFI_PROV_DEINIT:
                ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));

        /* Signal main application to continue execution */
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

static void wifi_start_softap(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

//    esp_netif_destroy(netif);
    netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &webconfig_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
            .ap = {
                    .max_connection = 3,
                    .ssid = CONFIG_MENJIN_WIFI_SSID,
                    .ssid_len = strlen(CONFIG_MENJIN_WIFI_SSID),
//                    .channel = FOLLOME2_ESP_WIFI_CHANNEL,
//                    .password = FOLLOME2_ESP_WIFI_PASS,
//                    .max_connection = FOLLOME2_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
                    .authmode = WIFI_AUTH_OPEN,
//                    .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
                    .authmode = WIFI_AUTH_WPA2_PSK,
#endif
                    .pmf_cfg = {
                            .required = true,
                    },
            },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s'", wifi_config.ap.ssid);
}

void webconfig_initialise_wifi(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &webconfig_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &webconfig_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &webconfig_event_handler, NULL));

    wifi_start_softap();

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    dns_server = start_dns_server(&config);

    start_captive_portal(CONFIG_BSP_SPIFFS_MOUNT_POINT);

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_EVENT, true, false, portMAX_DELAY);

    stop_dns_server(dns_server);

    esp_restart();
}