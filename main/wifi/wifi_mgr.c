//
// Created by Hessian on 2023/9/28.
//

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_check.h>
#include "wifi_mgr.h"
#include "captive_portal.h"

static EventGroupHandle_t s_wifi_event_group;

static const int WIFIMGR_CONNECTED_BIT = BIT0;

static const char *TAG = "wifi_mgr";

void wifi_mgr_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

}

void wifi_mgr_init_sta() {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

esp_err_t wifi_mgr_get_ip(char* ip)
{
    ip[0] = 0;
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    if (ret == ESP_OK) {
        sprintf(ip, IPSTR, IP2STR(&ip_info.ip));
    }

    return ret;
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFIMGR_CONNECTED_BIT);

        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));

        uint8_t ch = 0;
        wifi_second_chan_t secondChan;
        esp_wifi_get_channel(&ch, &secondChan);
        ESP_LOGI(TAG, "Connected with channel: %d - %d", ch, secondChan);

    } if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_WIFI_READY) {
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}

void wifi_mgr_start(void)
{

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* If device is not yet provisioned start provisioning service */
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning");

#if CONFIG_MENJIN_WIFI_CONFIG_SMARTCONFIG
        // will block until wifi connected
        smartconfig_initialise_wifi();
        start_captive_portal(CONFIG_BSP_SPIFFS_MOUNT_POINT);
#else
        webconfig_initialise_wifi();
#endif
    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_WIFI_READY, &event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL));

        wifi_mgr_init_sta();

        start_captive_portal(CONFIG_BSP_SPIFFS_MOUNT_POINT);
    }

    // block until connected
    xEventGroupWaitBits(s_wifi_event_group, WIFIMGR_CONNECTED_BIT, true, false, portMAX_DELAY);

    ESP_LOGI(TAG, "Wi-Fi connected");
}

//// WiFi scan

static void print_auth_mode(int authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OPEN");
            break;
        case WIFI_AUTH_OWE:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OWE");
            break;
        case WIFI_AUTH_WEP:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WEP");
            break;
        case WIFI_AUTH_WPA_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_PSK");
            break;
        case WIFI_AUTH_WPA2_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_PSK");
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_WPA2_PSK");
            break;
        case WIFI_AUTH_WPA2_ENTERPRISE:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_ENTERPRISE");
            break;
        case WIFI_AUTH_WPA3_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_PSK");
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_WPA3_PSK");
            break;
        default:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_UNKNOWN");
            break;
    }
}

static void print_cipher_type(int pairwise_cipher, int group_cipher)
{
    switch (pairwise_cipher) {
        case WIFI_CIPHER_TYPE_NONE:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_NONE");
            break;
        case WIFI_CIPHER_TYPE_WEP40:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP40");
            break;
        case WIFI_CIPHER_TYPE_WEP104:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP104");
            break;
        case WIFI_CIPHER_TYPE_TKIP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP");
            break;
        case WIFI_CIPHER_TYPE_CCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_CCMP");
            break;
        case WIFI_CIPHER_TYPE_TKIP_CCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
            break;
        case WIFI_CIPHER_TYPE_AES_CMAC128:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_AES_CMAC128");
            break;
        case WIFI_CIPHER_TYPE_SMS4:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_SMS4");
            break;
        case WIFI_CIPHER_TYPE_GCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP");
            break;
        case WIFI_CIPHER_TYPE_GCMP256:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP256");
            break;
        default:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
            break;
    }

    switch (group_cipher) {
        case WIFI_CIPHER_TYPE_NONE:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_NONE");
            break;
        case WIFI_CIPHER_TYPE_WEP40:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP40");
            break;
        case WIFI_CIPHER_TYPE_WEP104:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP104");
            break;
        case WIFI_CIPHER_TYPE_TKIP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP");
            break;
        case WIFI_CIPHER_TYPE_CCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_CCMP");
            break;
        case WIFI_CIPHER_TYPE_TKIP_CCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
            break;
        case WIFI_CIPHER_TYPE_SMS4:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_SMS4");
            break;
        case WIFI_CIPHER_TYPE_GCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP");
            break;
        case WIFI_CIPHER_TYPE_GCMP256:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP256");
            break;
        default:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
            break;
    }
}

/* Initialize Wi-Fi as sta and set scan method */
esp_err_t wifi_scan(uint16_t number, wifi_ap_record_t *ap_info, uint16_t *ap_count)
{
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&number, ap_info), TAG, "");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(ap_count), TAG, "");
    ESP_LOGI(TAG, "Total APs scanned = %u", *ap_count);

    for (int i = 0; (i < number) && (i < *ap_count); i++) {
        ESP_LOGI(TAG, "SSID \t\t%s", ap_info[i].ssid);
        ESP_LOGI(TAG, "RSSI \t\t%d", ap_info[i].rssi);
        print_auth_mode(ap_info[i].authmode);
        if (ap_info[i].authmode != WIFI_AUTH_WEP) {
            print_cipher_type(ap_info[i].pairwise_cipher, ap_info[i].group_cipher);
        }
        ESP_LOGI(TAG, "Channel \t\t%d", ap_info[i].primary);
    }

    return ESP_OK;
}