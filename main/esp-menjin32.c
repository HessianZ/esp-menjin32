
#include <sys/cdefs.h>
#include <stdlib.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "system/mqtt.h"
#include "system/settings.h"
#include "app_menjin.h"
#include "app_keys.h"
#include "wifi_mgr.h"
#include "bsp.h"

#define LED_PIN GPIO_NUM_15

static const char *TAG = "APP_MAIN";


typedef enum AppState {
    APP_WAITING_WIFI,
    APP_WAITING_IP,
    APP_RUNNING,
} AppState;

static AppState g_app_state = APP_WAITING_WIFI;

static void led_blink(int times, int interval_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(interval_ms / portTICK_PERIOD_MS);
        gpio_set_level(LED_PIN, 1);
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
    io_conf.pin_bit_mask = (1ULL<<LED_PIN);
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
                gpio_set_level(LED_PIN, 0);

                vTaskDelay(1000 / portTICK_PERIOD_MS);
                break;
            default:
                break;
        }
    }
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
    ESP_LOGI(TAG, "[APP] Free memory: %lu bytes", esp_get_free_heap_size());
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

    bsp_spiffs_mount();

    //start i2c task
    sys_param_t *settings = settings_get_parameter();

    if (settings->last_update_time > 0) {
        menjin_init();
    } else {
        ESP_LOGW(TAG, "System setting not initialized");

        // generate mqtt client id
        mqtt_client_id();
    }

    app_init_key_handles();

    xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);

    wifi_mgr_init();

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, NULL));

    // will block until wi-fi connected
    wifi_mgr_start();

    xTaskCreate(mqtt_task, "mqtt_task", 4096, NULL, 3, NULL);
    xTaskCreate(menjin_ring_detect_task, "menjin_ring_detect_task", 2048, NULL, 2, NULL);

    menjin_set_ring_callback(menjin_ring_callback);

    // 开启mDNS后会导致MQTT无法连接，暂时禁用
//    initialise_mdns();

}