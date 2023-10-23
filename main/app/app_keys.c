//
// Created by Hessian on 2023/7/31.
//

#include <gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>
#include "app_keys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "settings.h"
#include "app_menjin.h"

static const char *TAG = "APP_KEYS";

static xQueueHandle gpio_evt_queue = NULL;

#define BUTTON_TRIGGER_INTERVAL_MS 1000 // 防抖延迟时间，根据实际情况调整


// 定义按键信息的结构体
typedef struct {
    int pin; // 按键的GPIO引脚号
    TickType_t lastTriggerTime;
} ButtonInfo;

static ButtonInfo button_12;
static ButtonInfo button_13;
static ButtonInfo button_14;

// 按键有效为true，否则为false
bool button_trigger_limit(ButtonInfo* button) {
    if (button->lastTriggerTime == 0 || xTaskGetTickCount() - button->lastTriggerTime >= pdMS_TO_TICKS(BUTTON_TRIGGER_INTERVAL_MS)) {
        button->lastTriggerTime = xTaskGetTickCount();
        return true;
    }

    return false;
}

static void gpio_isr_handler(void *arg)
{
    ButtonInfo *buttonInfo = (ButtonInfo*) arg;
    if (button_trigger_limit(buttonInfo)) {
        xQueueSendFromISR(gpio_evt_queue, &buttonInfo, NULL);
    }
}

static void key_event_handle_task(void *arg)
{
    ButtonInfo *buttonInfo;

    sys_param_t *settings;

    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &buttonInfo, portMAX_DELAY)) {

            ESP_LOGI(TAG, "GPIO[%d] intr, val: %d", buttonInfo->pin, gpio_get_level(buttonInfo->pin));

            switch (buttonInfo->pin) {
                case GPIO_NUM_12:
                    ESP_LOGW(TAG, "Reset to default settings");
                    settings = settings_get_parameter();
                    sys_param_t default_settings = settings_get_default_parameter();
                    memcpy(settings, &default_settings, sizeof(sys_param_t));
                    settings_write_parameter_to_nvs();
                    ESP_LOGW(TAG, "Restarting the device");
                    esp_restart();
                    break;
                case GPIO_NUM_13:
                    ESP_LOGI(TAG, "PRESS KEY3 - KEY(Unlock)");
                    menjin_cmd_write(MENJIN_CMD_KEY3_UNLOCK);
                    break;
                case GPIO_NUM_14:
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

    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    //set as output mode
    io_conf.mode = GPIO_MODE_INPUT;
    //bit mask of the pins that you want to set,e.g.GPIO12/13/14
    io_conf.pin_bit_mask = ((1ULL<<GPIO_NUM_12) | (1ULL<<GPIO_NUM_13) | (1ULL<<GPIO_NUM_14));
    //disable pull-down mode
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //disable pull-up mode
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    //configure GPIO with the given settings

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    //start gpio task
    xTaskCreate(key_event_handle_task, "key_event_handle_task", 2048, NULL, 10, NULL);

    button_12.pin = GPIO_NUM_12;
    button_13.pin = GPIO_NUM_13;
    button_14.pin = GPIO_NUM_14;

    //install gpio isr service
    gpio_install_isr_service(0);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_NUM_12, gpio_isr_handler, &button_12);
    gpio_isr_handler_add(GPIO_NUM_13, gpio_isr_handler, &button_13);
    gpio_isr_handler_add(GPIO_NUM_14, gpio_isr_handler, &button_14);
}
