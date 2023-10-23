//
// Created by Hessian on 2023/7/29.
//

#include <sys/cdefs.h>
#include <string.h>
#include <stdlib.h>
#include <gpio.h>
#include <mdns.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "system/smartconfig.h"
#include "system/mqtt.h"
#include "system/settings.h"
#include "http_server.h"
#include "app_menjin.h"
#include "app_keys.h"

static const char *TAG = "APP_MAIN";

static void init_wifi_task(void *param)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    smartconfig_init(NULL);
    vTaskDelete(NULL);
}

typedef enum AppState {
    APP_WAITING_WIFI,
    APP_WAITING_IP,
    APP_RUNNING,
} AppState;

static AppState g_app_state = APP_WAITING_WIFI;

static void led_blink(int times, int interval_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(GPIO_NUM_2, 0);
        vTaskDelay(interval_ms / portTICK_PERIOD_MS);
        gpio_set_level(GPIO_NUM_2, 1);
        vTaskDelay(interval_ms / portTICK_PERIOD_MS);
    }
}

_Noreturn static void led_task(void *param)
{
    gpio_config_t io_conf;

    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO15/16
    io_conf.pin_bit_mask = (1ULL<<GPIO_NUM_2);
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    while (1) {
        switch (g_app_state) {
            case APP_WAITING_WIFI:
                led_blink(3, 500);
                break;
            case APP_WAITING_IP:
                led_blink(10, 100);
                break;
            case APP_RUNNING:
                gpio_set_level(GPIO_NUM_2, 0);

                vTaskDelay(1000 / portTICK_PERIOD_MS);
                break;
            default:
                break;
        }
    }
}

static bool mdns_initialized = false;

static void initialise_mdns()
{
    if (mdns_initialized) {
        return;
    }
    ESP_LOGI(TAG, "Initializing mDNS...");
    char hostname[] = "esp-menjin";
    //initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    //set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK( mdns_hostname_set(hostname) );
    ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);
//    //set default mDNS instance name
//    ESP_ERROR_CHECK( mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE) );

    // esp chip id
    uint8_t chipId[6];
    esp_read_mac(chipId, ESP_MAC_WIFI_STA);
    // chip id hex string
    char chipIdHex[13];
    sprintf(chipIdHex, "%02x%02x%02x%02x%02x%02x", chipId[0], chipId[1], chipId[2], chipId[3], chipId[4], chipId[5]);

    //structure with TXT records
    mdns_txt_item_t serviceTxtData[4] = {
            {"chipId", chipIdHex},
            {"version", "1.0"},
            {"board", "esp8266"},
            {"vendor", "witgine tech"},
    };

    //initialize service
    ESP_ERROR_CHECK( mdns_service_add("menjin-api", "_http", "_tcp", 80, serviceTxtData, 4) );

    mdns_initialized = true;

    ESP_LOGI(TAG, "mDNS Initialize finished");
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "WiFi disconnected ...");
    g_app_state = APP_WAITING_IP;
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "WiFi connected ...");
    g_app_state = APP_RUNNING;
}

static void menjin_ring_callback(void)
{
    ESP_LOGI(TAG, "Menjin ring callback");

    mqtt_notify("ring");
}

void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    esp_log_level_set("MQTT", ESP_LOG_VERBOSE);
    esp_log_level_set("smartconfig", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(settings_read_parameter_from_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, NULL));

    //start i2c task
    menjin_init();

    app_init_key_handles();

    xTaskCreate(init_wifi_task, "init_wifi_task", 4096, NULL, 3, NULL);
    xTaskCreate(mqtt_task, "mqtt_task", 4096, NULL, 3, NULL);
    xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);
    xTaskCreate(menjin_ring_detect_task, "menjin_ring_detect_task", 2048, NULL, 2, NULL);

    menjin_set_ring_callback(menjin_ring_callback);

    // 开启mDNS后会导致MQTT无法连接，暂时禁用
//    initialise_mdns();

    http_server_init();
}

