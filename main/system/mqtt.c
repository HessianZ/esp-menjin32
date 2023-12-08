//
// Created by Hessian on 2023/7/29.
//
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <esp_tls.h>
#include <esp_mac.h>
#include <esp_random.h>
#include "esp_system.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "settings.h"
#include "app_menjin.h"
#include "mqtt.h"
#include "wifi_mgr.h"

#define MQTT_TOPIC_PREFIX "menjin/"

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t g_client;
static char g_topic_cmd[64];
static char g_topic_notify[64];

extern const uint8_t server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const uint8_t server_root_cert_pem_end[]   asm("_binary_server_root_cert_pem_end");

void mqtt_handle_menjin_cmd(char *payload, int len)
{
    if (len == 0) {
        return;
    }

    if (strncmp(payload, "cmd ", 4) == 0) {
        char cmd_str[4] = {0};
        strncpy(cmd_str, payload + 4, len -4);
        uint8_t cmd = atoi(cmd_str);
        esp_err_t ret = menjin_cmd_write(cmd);
        ESP_LOGI(TAG, "[menjin] do cmd: %d(%s), ret: %d", cmd, cmd_str, ret);
    } else if (strncmp(payload, "open", len) == 0) {
        ESP_LOGI(TAG, "mqtt open start");
        // 接听的时候会有声音，会导致触发ring_callback，这里先干掉callback，解锁完再解开
        void *cb = menjin_get_ring_callback();
        menjin_set_ring_callback(NULL);
        menjin_cmd_write(MENJIN_CMD_KEY4_SPEAKER);
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        menjin_cmd_write(MENJIN_CMD_KEY3_UNLOCK);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        menjin_cmd_write(MENJIN_CMD_KEY4_SPEAKER);
        menjin_set_ring_callback(cb);
        ESP_LOGI(TAG, "mqtt open end");
    } else {
        ESP_LOGW(TAG, "[menjin] unknown cmd: %.*s", len, payload);
    }
}


static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;

    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

            char json[128] = {0};
            char ip[15] = {0};
            wifi_mgr_get_ip(ip);
            sprintf(json, "{\"action\":\"%s\",\"deviceId\":\"%s\",\"localIp\":\"%s\"}", "online", mqtt_client_id(), ip);

            msg_id = esp_mqtt_client_publish(client, g_topic_notify, json, 0, 1, 0);
            ESP_LOGI(TAG, "publish %s to %s successful, msg_id=%d", json, g_topic_notify, msg_id);

            msg_id = esp_mqtt_client_subscribe(client, g_topic_cmd, 0);
            ESP_LOGI(TAG, "subscribe %s successful, msg_id=%d", g_topic_cmd, msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
//            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
//            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
//            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "Receive [%.*s] DATA: %.*s", event->topic_len, event->topic, event->data_len, event->data);

            mqtt_handle_menjin_cmd(event->data, event->data_len);

            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }

    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGI(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    mqtt_event_handler_cb(event_data);
}

int generate_mqtt_client_id(char *id)
{
    id[0] = '\0';
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t rand = esp_random();
    uint8_t *prand = (uint8_t *) &rand;
    return sprintf(id, "MJ%02x%02X%02X%02x%02X%02X-%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], *prand, *(prand+1), *(prand+2), *(prand+3));
}

char *mqtt_client_id()
{
    sys_param_t *settings = settings_get_parameter();
    if (strlen(settings->mqtt_client_id) == 0) {
        ESP_LOGW(TAG, "mqtt_client_id is empty, generate new one");
        generate_mqtt_client_id(settings->mqtt_client_id);
        settings_write_parameter_to_nvs();
        settings_dump();
    }

    return settings->mqtt_client_id;
}

void mqtt_task(void *pvParameters)
{
    g_topic_cmd[0] = '\0';
    g_topic_notify[0] = '\0';

    sys_param_t *settings = settings_get_parameter();

    if (strlen(settings->mqtt_url) == 0) {
        ESP_LOGE(TAG, "mqtt_url is empty, mqtt client will not start");
        vTaskDelete(NULL);
        return;
    }

    char *client_id = mqtt_client_id();

    sprintf(g_topic_cmd, MQTT_TOPIC_PREFIX "%s/cmd", client_id);
    sprintf(g_topic_notify, MQTT_TOPIC_PREFIX "%s/notify", client_id);


#if CONFIG_IDF_TARGET_ESP8266
    esp_mqtt_client_config_t mqtt_cfg = {
        .client_id = g_client_id,
        .uri = settings->mqtt_url,
        .cert_pem = (char *) &server_root_cert_pem_start,
        .cert_len = server_root_cert_pem_end - server_root_cert_pem_start,
        .skip_cert_common_name_check = true,
    };

    if (strlen(settings->mqtt_username) > 0) {
        mqtt_cfg.username = settings->mqtt_username;
    }

    if (strlen(settings->mqtt_password) > 0) {
        mqtt_cfg.password = settings->mqtt_password;
    }
#else
    esp_mqtt_client_config_t mqtt_cfg = {
            .broker = {
                    .address.uri = settings->mqtt_url
            },
            .credentials = {
                    .client_id = client_id,
            }
    };

    if (strlen(settings->mqtt_username) > 0) {
        mqtt_cfg.credentials.username = settings->mqtt_username;
    }

    if (strlen(settings->mqtt_password) > 0) {
        mqtt_cfg.credentials.authentication.password = settings->mqtt_password;
    }
#endif

    if (strncmp(settings->mqtt_url, "mqtts://", 8) == 0
        && (
                strstr(settings->mqtt_url, "witgine.com") != NULL
                || strstr(settings->mqtt_url, "emqx") != NULL
            )
    ) {
        mqtt_cfg.broker.verification.certificate = (const char *) server_root_cert_pem_start;
    }

    ESP_LOGI(TAG, "mqtt_cfg.uri: %s", settings->mqtt_url);
    ESP_LOGI(TAG, "mqtt_cfg.username: %s", settings->mqtt_username);
    ESP_LOGI(TAG, "mqtt_cfg.password: %s", settings->mqtt_password);

    g_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, g_client);
    esp_mqtt_client_start(g_client);
    ESP_LOGI(TAG, "mqtt_client started");

    vTaskDelete(NULL);
}

void mqtt_notify(char* content)
{
    esp_mqtt_client_publish(g_client, g_topic_notify, content, 0, 1, 0);
}