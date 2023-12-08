#include <esp_types.h>
#include <sys/cdefs.h>
//
// Created by Hessian on 2023/7/30.
//
#include <esp_adc/adc_oneshot.h>
#include <driver/i2c.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "settings.h"


static const char *TAG = "APP_MENJIN";

#define I2C_MASTER_TX_BUF_DISABLE   0                 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0                 /*!< I2C master doesn't need buffer */

// I2C_A 门禁主控
#define MENJIN_I2C_SCL_PIN           40               /*!< gpio number for I2C master clock */
#define MENJIN_I2C_SDA_PIN           39               /*!< gpio number for I2C master data  */
#define MENJIN_I2C_NUM               I2C_NUM_0        /*!< I2C port number for master dev */

// I2C_B 触摸键盘
#define KEYBOARD_I2C_SCL_PIN         2                /*!< gpio number for I2C master clock */
#define KEYBOARD_I2C_SDA_PIN         3                /*!< gpio number for I2C master data  */
#define KEYBOARD_I2C_NUM             I2C_NUM_1        /*!< I2C port number for master dev */
#define I2C_SLAVE_TX_BUF_LEN         256
#define I2C_SLAVE_RX_BUF_LEN         256

#define WRITE_BIT                    I2C_MASTER_WRITE /*!< I2C master write */
#define READ_BIT                     I2C_MASTER_READ  /*!< I2C master read */
#define ACK_CHECK_EN                 0x1              /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS                0x0              /*!< I2C master will not check ack from slave */
#define ACK_VAL                      0x0              /*!< I2C ack value */
#define NACK_VAL                     0x1              /*!< I2C nack value */
#define LAST_NACK_VAL                0x2              /*!< I2C last_nack value */

// 门铃检测相关
#define RING_DETECT_INTERVAL         200              // 检测门铃输入的间隔，单位：毫秒
#define ADC_SAMPLE_TIMES             5               // ADC采样次数
#define ADC_SAMPLE_INTERVAL          20              // ADC采样间隔，单位：毫秒
#define ADC_VALUE_MAX                6000            // ADC采样值的最大值

#define CALLBACK_INTERVAL_MS (30 * 1000) // 回调函数的调用频率，单位：毫秒

static void (*g_ring_callback)(void) = NULL; // ADC输入的回调函数

/**
 * @brief i2c master initialization
 */
static esp_err_t menjin_i2c_init(void)
{
    int i2c_master_port = MENJIN_I2C_NUM;

    sys_param_t *settings = settings_get_parameter();

    i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = MENJIN_I2C_SDA_PIN,
            .scl_io_num = MENJIN_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = settings->i2c_clock
    };

    i2c_param_config(i2c_master_port, &conf);

    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/**
 * @brief i2c slave initialization
 */
static esp_err_t keyboard_i2c_init(void)
{
    sys_param_t *settings = settings_get_parameter();

    int i2c_port = KEYBOARD_I2C_NUM;

    i2c_config_t conf = {
            .mode = I2C_MODE_SLAVE,
            .sda_io_num = KEYBOARD_I2C_SDA_PIN,
            .scl_io_num = KEYBOARD_I2C_SCL_PIN,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .slave.slave_addr = settings->i2c_address,
            .slave.maximum_speed = settings->i2c_clock,
    };

    i2c_param_config(i2c_port, &conf);

    return i2c_driver_install(i2c_port, conf.mode, I2C_SLAVE_RX_BUF_LEN, I2C_SLAVE_TX_BUF_LEN, 0);
}

_Noreturn static void keyboard_i2c_read_task(void *param);


/**
 * @brief i2c master initialization
 */
esp_err_t menjin_init()
{
    ESP_LOGI(TAG, "Initializing Menjin...");

    ESP_ERROR_CHECK(menjin_i2c_init());
    ESP_ERROR_CHECK(keyboard_i2c_init());

    xTaskCreate(keyboard_i2c_read_task, "keyboard_i2c_read_task", 2048, NULL, 10, NULL);

    return ESP_OK;
}

esp_err_t menjin_stop()
{
    return i2c_driver_delete(MENJIN_I2C_NUM);
}

/**
 * @brief code to write by i2c
 *
 * 1. send data
 * _______________________________________________________________________
 * |  start | slave_addr + wr_bit + ack | write data byte + ack  | stop |
 * |--------|---------------------------|------------------------|------|
 *
 * @param i2c_num I2C port number
 * @param data data to send
 * @param data_len data length
 *
 * @return
 *     - ESP_OK Success
 *     - ESP_ERR_INVALID_ARG Parameter error
 *     - ESP_FAIL Sending command error, slave doesn't ACK the transfer.
 *     - ESP_ERR_INVALID_STATE I2C driver not installed or not in master mode.
 *     - ESP_ERR_TIMEOUT Operation timeout because the bus is busy.
 */
esp_err_t menjin_cmd_write(uint8_t data)
{
    sys_param_t *settings = settings_get_parameter();

    int ret;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, settings->i2c_address << 1 | WRITE_BIT, ACK_CHECK_DIS);
    i2c_master_write_byte(cmd, data, ACK_CHECK_DIS);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(MENJIN_I2C_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    ESP_LOGI(TAG, "menjin_cmd_write[0x%02x]: 0x%02x", settings->i2c_address, data);

    return ret;
}

_Noreturn static void keyboard_i2c_read_task(void *param)
{
    uint8_t cmd;
    sys_param_t *settings = settings_get_parameter();

    while (1) {
        int ret = i2c_slave_read_buffer(KEYBOARD_I2C_NUM, &cmd, 1, 1000 / portTICK_PERIOD_MS);

        // write cmd to write queue
        if (ret > 0) {
            ESP_LOGI(TAG, "keyboard_i2c_read_task[0x%02x] RET: %d", settings->i2c_address, ret);
            menjin_cmd_write(cmd);
        }
    }
}

void menjin_set_ring_callback(void (*callback)(void)) {
    g_ring_callback = callback;
}

void* menjin_get_ring_callback(void) {
    return g_ring_callback;
}

// ADC输入检测任务
_Noreturn void menjin_ring_detect_task(void* pvParameters) {
    TickType_t lastCallbackTime = 0;
    int adcValues[ADC_SAMPLE_TIMES] = {0};
    int adcValueAvg = 0;
    esp_err_t err;

    // 1. init adc
    adc_oneshot_unit_handle_t adc1_handle;

    adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    sys_param_t *settings = settings_get_parameter();

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    while (1) {

        // 防抖动
        for (int i = 0; i < ADC_SAMPLE_TIMES; ++i) {
            // 读取ADC输入值
            err = adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adcValues[i]);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ADC read failed: %d", err);
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_INTERVAL));
        }

        // calc average value of adc
        adcValueAvg = 0;
        for (int i = 0; i < ADC_SAMPLE_TIMES; ++i) {
            // 异常高值处理
            if (adcValues[i] > ADC_VALUE_MAX) {
                adcValues[i] = settings->ring_adc_threshold;
            }
            adcValueAvg += adcValues[i];
        }
        adcValueAvg /= ADC_SAMPLE_TIMES;


        // 检查ADC输入值是否大于50
        if (adcValueAvg > settings->ring_adc_threshold) {
            ESP_LOGI(TAG, "ADC avg value: %d", adcValueAvg);
            // 检查是否满足回调函数调用频率限制
            if (xTaskGetTickCount() - lastCallbackTime >= pdMS_TO_TICKS(CALLBACK_INTERVAL_MS)) {
                // 调用回调函数
                if (g_ring_callback != NULL) {
                    g_ring_callback();
                }

                // 更新最后回调时间
                lastCallbackTime = xTaskGetTickCount();
            }
        }

        // 一定的延迟，避免过于频繁的检测
        vTaskDelay(pdMS_TO_TICKS(RING_DETECT_INTERVAL));
    }

    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
}
