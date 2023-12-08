//
// Created by Hessian on 2023/7/31.
//

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>
#include <esp_wifi.h>
#include "app_keys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "settings.h"
#include "app_menjin.h"
#include "iot_button.h"

static const char *TAG = "APP_KEYS";

static QueueHandle_t gpio_evt_queue = NULL;

#define BUTTON_TRIGGER_INTERVAL_MS 1000 // 防抖延迟时间，根据实际情况调整

// 定义按键信息的结构体
typedef struct {
    int pin; // 按键的GPIO引脚号
    TickType_t lastTriggerTime;
} ButtonInfo;

#define K0_PIN GPIO_NUM_0
#define K1_PIN GPIO_NUM_6
#define K2_PIN GPIO_NUM_7
#define K3_PIN GPIO_NUM_4
#define K4_PIN GPIO_NUM_5
#define KEY_COUNT 5

static int keys[] = {
        K0_PIN,
        K1_PIN,
        K2_PIN,
        K3_PIN,
        K4_PIN,
};
button_handle_t gpio_btn[KEY_COUNT];

static void button_click_cb(void *arg, void *usr_data)
{
    xQueueSendFromISR(gpio_evt_queue, usr_data, NULL);
}

static void key_event_handle_task(void *arg)
{
    int keyPin;

    sys_param_t *settings;

    esp_err_t ret;

    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &keyPin, portMAX_DELAY)) {
            switch (keyPin) {
                case K0_PIN:
                    ESP_LOGI(TAG, "PRESS KEY0 - KEY(boot)");
                    ESP_LOGW(TAG, "Reset to default settings");
                    settings = settings_get_parameter();
                    sys_param_t default_settings = settings_get_default_parameter();
                    memcpy(settings, &default_settings, sizeof(sys_param_t));
                    settings_write_parameter_to_nvs();
//                    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
//                    wifi_config_t wifi_cfg = {0};
//                    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
                    ret = esp_wifi_restore();
                    ESP_LOGI(TAG, "Wi-Fi restore: %d", ret);

                    ESP_LOGW(TAG, "Restarting the device");
                    esp_restart();
                    break;
                case K2_PIN:
                    ESP_LOGI(TAG, "PRESS KEY2 - KEY(XX)");
                    break;
                case K3_PIN:
                    ESP_LOGI(TAG, "PRESS KEY3 - KEY(Unlock)");
                    menjin_cmd_write(MENJIN_CMD_KEY3_UNLOCK);
                    break;
                case K4_PIN:
                    ESP_LOGI(TAG, "PRESS KEY4 - SPEAKER(Hand Free)");
                    menjin_cmd_write(MENJIN_CMD_KEY4_SPEAKER);
                    break;
                default:
                    break;
            }
        }
    }
}

void app_init_key_handles(void)
{
    ESP_LOGI(TAG, "app_init_key_handles");

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    button_config_t gpio_btn_cfg = {
            .type = BUTTON_TYPE_GPIO,
            .long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
            .short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
            .gpio_button_config = {
                    .gpio_num = K0_PIN,
                    .active_level = 0,
            },
    };

    for (int i = 0; i < KEY_COUNT; ++i) {
        gpio_btn_cfg.gpio_button_config.gpio_num = keys[i];
        gpio_btn[i] = iot_button_create(&gpio_btn_cfg);

        if (i == 0) {
            iot_button_register_cb(gpio_btn[i], BUTTON_LONG_PRESS_UP, button_click_cb, &keys[i]);
        } else {
            iot_button_register_cb(gpio_btn[i], BUTTON_SINGLE_CLICK, button_click_cb, &keys[i]);
        }
    }

    //start gpio task
    xTaskCreate(key_event_handle_task, "key_event_handle_task", 2048, NULL, 10, NULL);
}
