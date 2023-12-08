//
// Created by Hessian on 2023/9/6.
//

#include <esp_log.h>
#include <esp_wifi_types.h>
#include <esp_wifi.h>
#include <ctype.h>
#include <sys/stat.h>
#include <esp_check.h>
#include <lwip/sockets.h>
#include <cJSON.h>
#include <esp_chip_info.h>
#include "captive_portal.h"
#include "wifi_mgr.h"
#include "settings.h"
#include "json_parser.h"
#include "app_menjin.h"
#include "mqtt.h"

static const char *TAG = "CAPTIVE_PORTAL";

#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)


#define FILE_PATH_MAX 255
#define HTML_BUF_SIZE 2048
#define SCRATCH_BUFSIZE 10240
#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static httpd_handle_t server = NULL;


typedef struct rest_server_context {
    char base_path[FILE_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filename, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filename, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filename, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filename, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filename, ".jpg")) {
        type = "image/jpeg";
    } else if (CHECK_FILE_EXTENSION(filename, ".gif")) {
        type = "image/gif";
    } else if (CHECK_FILE_EXTENSION(filename, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filename, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

static bool handle_captive_portal_request(httpd_req_t *req)
{
    char host[128] = {0};
    // get http host from request
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get http host");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "http host: %s", host);

    bool handled = false;
    // see https://en.wikipedia.org/wiki/Captive_portal
    if (strcmp(host, "captive.apple.com") == 0
        || strcmp(host, "captive.apple.com") == 0) {
        // ios
        httpd_resp_send(req, "<script type='text/javascript'>location = '/index.html'</script>", -1);
        handled = true;
    } else if (strcmp(host, "connectivitycheck.gstatic.com") == 0
               || strcmp(host, "clients3.google.com") == 0) {
        // android
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, "", 0);
    } else if (strcmp(host, "www.msftncsi.com") == 0) {
        // windows
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, "", 0);
    } else if (strcmp(host, "nmcheck.gnome.org") == 0) {
        // NetworkManager
        httpd_resp_send(req, "NetworkManager is online", -1);
    }

    if (handled) {
        ESP_LOGI(TAG, "Captive portal request handled: %s %s%s", http_method_str(req->method), host, req->uri);
    }

    return handled;
}

static esp_err_t send_file_response(httpd_req_t *req, char* filename)
{
    esp_err_t ret = ESP_OK;
    FILE *fp = NULL;
    char *buf = NULL;
    size_t read_len = 0;

    fp = fopen(filename, "rb");

    ESP_GOTO_ON_FALSE(fp != NULL, ESP_ERR_NOT_FOUND, err, TAG, "Failed to open file %s", filename);

    buf = calloc(HTML_BUF_SIZE, sizeof(char));

    if (NULL == buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for buf");
        return ESP_ERR_NO_MEM;
    }

    set_content_type_from_file(req, filename);

    while ((read_len = fread(buf, sizeof(char), HTML_BUF_SIZE, fp)) > 0) {
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, buf, read_len), err, TAG, "Failed to send file");
    }

    ESP_GOTO_ON_FALSE(feof(fp), ESP_FAIL, err, TAG, "Failed to read file %s error: %d", filename, ferror(fp));

    // chunks send finish
    httpd_resp_send_chunk(req, NULL, 0);

    err:
    if (fp != NULL) {
        fclose(fp);
    }
    if (buf != NULL) {
        free(buf);
    }

    if (ret == ESP_ERR_NOT_FOUND) {
        ret = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
    }

    return ret;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        // try to handle captive portal request for root request.
        if (handle_captive_portal_request(req)) {
            return ESP_OK;
        }
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    return send_file_response(req, filepath);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    uint16_t number = 10;
    uint16_t size = number * sizeof(wifi_ap_record_t);
    wifi_ap_record_t *ap_info = malloc(number * sizeof(wifi_ap_record_t));
    uint16_t ap_count = 0;
    char *resp = NULL;
    char json_obj[128];
    uint16_t resp_buf_len;

    memset(ap_info, 0, size);

    ESP_GOTO_ON_ERROR(wifi_scan(number, ap_info, &ap_count), end, TAG, "wifi_scan failed");

    httpd_resp_set_type(req, "text/html");

    if (ap_count > 0) {
        resp_buf_len = ap_count * (sizeof(ap_info->ssid) + 22);
        resp = malloc(resp_buf_len);
        memset(resp, 0, resp_buf_len);

        resp[0] = '[';

        int resp_len = 1;
        for (int i = 0; (i < number) && (i < ap_count); i++) {
            resp_len += sprintf(json_obj, "{\"ssid\":\"%s\", \"rssi\":%d}", ap_info[i].ssid, ap_info[i].rssi);
            if (i > 0) {
                strcat(resp, ",");
                resp_len += 1;
            }
            strcat(resp, json_obj);
        }
        strcat(resp, "]");
//        resp[resp_len+1] = '\0';
        resp_len += 1;
        ESP_LOGD(TAG, "resp: %s", resp);
        ESP_RETURN_ON_ERROR(httpd_resp_send(req, resp, resp_len), TAG, "send response failed");
        free(resp);
    } else {
        ESP_RETURN_ON_ERROR(httpd_resp_sendstr(req, "[]"), TAG, "send response failed");
    }

    end:
    free(ap_info);

    return ret;
}

static esp_err_t captive_portal_handler(httpd_req_t *req)
{

    // try to handle captive portal request
    if (handle_captive_portal_request(req)) {
        return ESP_OK;
    }

    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/index.html");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_sendstr(req, "Redirect to the captive portal");

    ESP_LOGI(TAG, "Redirecting to /index.html");
    return ESP_OK;
}

void url_decode(char *src) {
    char *dst = src;
    char a, b;

    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((uint8_t)a) && isxdigit((uint8_t)b))) {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}


static void async_wifi_connect(void* arg)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    // wifi connect will be auto called after start
    // see app_wifi event handler function.
    // ESP_ERROR_CHECK(esp_wifi_connect());

    vTaskDelete(NULL);
}

static const char* settings_to_json(cJSON *root)
{
    wifi_config_t wifiConfig;
    esp_wifi_get_config(WIFI_IF_STA, &wifiConfig);
    sys_param_t *settings = settings_get_parameter();

    cJSON_AddStringToObject(root, "wifi_ssid", (char*)wifiConfig.sta.ssid);
    cJSON_AddStringToObject(root, "wifi_password", (char*)wifiConfig.sta.password);
    cJSON_AddNumberToObject(root, "wifi_channel", wifiConfig.sta.channel);
    // add mqtt configs
    cJSON_AddStringToObject(root, "mqtt_client_id", settings->mqtt_client_id);
    cJSON_AddStringToObject(root, "mqtt_url", settings->mqtt_url);
    cJSON_AddStringToObject(root, "mqtt_username", settings->mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_password", settings->mqtt_password);
    // add i2c configs
    cJSON_AddNumberToObject(root, "i2c_clock", settings->i2c_clock);
    cJSON_AddNumberToObject(root, "i2c_address", settings->i2c_address);
    // add other configs
    cJSON_AddNumberToObject(root, "ring_adc_threshold", settings->ring_adc_threshold);
    const char *sys_info = cJSON_Print(root);

    return sys_info;
}

static esp_err_t config_get_handler(httpd_req_t  *req)
{
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    const char *sys_info = settings_to_json(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);

    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(300 / portTICK_PERIOD_MS);
    esp_restart();
}

esp_err_t api_handler_restart(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Restarting the device");

    httpd_resp_send(req, "ok", 2);
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *)(req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';
    ESP_LOGI(TAG, "Request body: %s", buf);

    esp_err_t ret = ESP_OK;

    jparse_ctx_t *jctx = NULL;
    jctx = (jparse_ctx_t *)malloc(sizeof(jparse_ctx_t));
    ret = json_parse_start(jctx, buf, req->content_len);
    if (ret != OS_SUCCESS) {
        ESP_LOGE(TAG, "json_parse_start failed\n");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json parse failed");
        return ESP_FAIL;
    }
    sys_param_t *settings = settings_get_parameter();
    char str_val[128] = {0};
    if (json_obj_get_string(jctx, "mqtt_url", (char *) &str_val, sizeof(settings->mqtt_url)) == OS_SUCCESS) {
        strcpy(settings->mqtt_url, str_val);
    }
    if (json_obj_get_string(jctx, "mqtt_username", (char *) &str_val, sizeof(settings->mqtt_username)) == OS_SUCCESS) {
        strcpy(settings->mqtt_username, str_val);
    }
    if (json_obj_get_string(jctx, "mqtt_password", (char *) &str_val, sizeof(settings->mqtt_password)) == OS_SUCCESS) {
        strcpy(settings->mqtt_password, str_val);
    }
    int int_val;
    if (json_obj_get_int(jctx, "i2c_clock", &int_val) == OS_SUCCESS) {
        settings->i2c_clock = int_val;
    }
    if (json_obj_get_int(jctx, "i2c_address", &int_val) == OS_SUCCESS) {
        settings->i2c_address = int_val;
    }
    if (json_obj_get_int(jctx, "ring_adc_threshold", &int_val) == OS_SUCCESS) {
        settings->ring_adc_threshold = int_val;
    }

    wifi_config_t wifi_cfg = {0};
    json_obj_get_string(jctx, "wifi_ssid", (char *) &wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid));
    json_obj_get_string(jctx, "wifi_password", (char *) &wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password));

    // 至少要有wifi ssid
    if (strlen((char*)wifi_cfg.sta.ssid) > 0) {
        ESP_LOGI(TAG, "WiFi settings accepted!");

        httpd_resp_set_type(req, "text/html");
        if (esp_wifi_set_storage(WIFI_STORAGE_FLASH) == ESP_OK &&
            esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi settings applied and stored to flash");
            settings_dump();
            if (settings_write_parameter_to_nvs() != ESP_OK) {
                ESP_LOGE(TAG, "Save settings failed");
                ret = httpd_resp_sendstr(req, "保存设置失败，无法写入NVS");
            } else {
                ESP_LOGW(TAG, "Restarting the device");
                ret = httpd_resp_sendstr(req, "ok");
                xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
            }

            // Response with new settings
//    cJSON *root = cJSON_CreateObject();
//    const char *sys_info = settings_to_json(root);
//    httpd_resp_sendstr(req, sys_info);
//    free((void *)sys_info);
//    cJSON_Delete(root);

//            xTaskCreate(async_wifi_connect, "app_wifi_connect", 4096, NULL, 5, NULL);
        } else {
            ESP_LOGE(TAG, "Failed to set WiFi config to flash");
            ret = httpd_resp_sendstr(req, "写入WiFi信息失败");
        }
    } else {
        ESP_LOGW(TAG, "WiFi settings rejected!");
        ret = httpd_resp_sendstr(req, "缺少参数：必须提供WiFi SSID");
    }

    return ret;
}

esp_err_t api_handler_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Resetting the device");

    sys_param_t *settings = settings_get_parameter();
    sys_param_t default_settings = settings_get_default_parameter();
    memcpy(settings, &default_settings, sizeof(sys_param_t));
    settings_write_parameter_to_nvs();
//                    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
//                    wifi_config_t wifi_cfg = {0};
//                    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_restore();

    httpd_resp_send(req, "ok", 2);
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t api_mqtt_client_id_get_handler(httpd_req_t *req)
{
    char *id = mqtt_client_id();
    httpd_resp_send(req, id, strlen(id));

    return ESP_OK;
}

esp_err_t api_handler_menjin_cmd(httpd_req_t *req)
{
#define OK_STR "ok"
    ESP_LOGI(TAG, "/api/menjin/cmd handler read content length %d", req->content_len);

    char *resp_str = OK_STR;

    sys_param_t *settings = settings_get_parameter();

    // read cmd param from query string
    char buf[128];
    int ret = httpd_req_get_url_query_str(req, buf, sizeof(buf));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[api_handler_menjin_cmd] Found URL query => %s", buf);
        char param[16];
        if (httpd_query_key_value(buf, "cmd", param, sizeof(param)) == ESP_OK) {
            int cmd = atoi(param);

            esp_err_t cmd_ret = ESP_OK;

            if (cmd >= MENJIN_CMD_KEY4_SPEAKER && cmd <= MENJIN_CMD_KEY1) {
                cmd_ret = menjin_cmd_write(cmd);
                ESP_LOGI(TAG, "[api_handler_menjin_cmd] menjin_cmd_write(%d) => %d", cmd, cmd_ret);
            } else if (strcmp(param, "open") == 0) {
                cmd_ret = menjin_cmd_write(MENJIN_CMD_KEY4_SPEAKER);
                ESP_LOGI(TAG, "[api_handler_menjin_cmd] menjin_cmd_write(%d) => %d", MENJIN_CMD_KEY4_SPEAKER, cmd_ret);
                cmd_ret = menjin_cmd_write(MENJIN_CMD_KEY3_UNLOCK);
                ESP_LOGI(TAG, "[api_handler_menjin_cmd] menjin_cmd_write(%d) => %d", MENJIN_CMD_KEY3_UNLOCK, cmd_ret);
                cmd_ret = menjin_cmd_write(MENJIN_CMD_KEY4_SPEAKER);
                ESP_LOGI(TAG, "[api_handler_menjin_cmd] menjin_cmd_write(%d) => %d", MENJIN_CMD_KEY4_SPEAKER, cmd_ret);
            } else if (strcmp(param, "set_clock") == 0) {
                if (httpd_query_key_value(buf, "value", param, sizeof(param)) == ESP_OK) {
                    int clock = atoi(param);
                    if (clock <= 0 || clock > 1000000) {
                        httpd_resp_set_status(req, "400 Bad Request");
                        resp_str = "param 'value' exceeds range";
                        ESP_LOGW(TAG, "[api_handler_menjin_cmd] request menjin set_clock param 'value' exceeds range: %d", clock);
                    } else {
                        int old_clock = settings->i2c_clock;
                        settings->i2c_clock = clock;
                        cmd_ret = settings_write_parameter_to_nvs();
                        if (cmd_ret != ESP_OK) {
                            ESP_LOGE(TAG, "[api_handler_menjin_cmd] set_clock settings_write_parameter_to_nvs failed: %d", cmd_ret);
                        }
                        ESP_LOGI(TAG, "[api_handler_menjin_cmd] set_clock %d => %d", old_clock, clock);
                    }
                } else {
                    httpd_resp_set_status(req, "400 Bad Request");
                    resp_str = "param 'value' not found";
                    ESP_LOGW(TAG, "[api_handler_menjin_cmd] request menjin set_clock without value param");
                }
            }
            buf[0] = '\0';

            if (cmd_ret != ESP_OK) {
                httpd_resp_set_status(req, "500 Server Internal Error");
                sprintf(buf, "menjin_cmd_write failed: %d", cmd_ret);
                resp_str = (char *) &buf;
            }

        } else {
            httpd_resp_set_status(req, "500 Server Internal Error");
            resp_str = "param 'cmd' not found";
        }


    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        resp_str = "param 'cmd' not found";
        ESP_LOGW(TAG, "[api_handler_menjin_cmd] request menjin cmd without query string");
    }

    httpd_resp_send(req, resp_str, strlen(resp_str));

    return ESP_OK;
#undef OK_STR
}


esp_err_t start_captive_portal(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 3;
    config.lru_purge_enable = true;
    config.max_resp_headers = 30;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t uri_handlers[] = {
            {
                .uri = "/hotspot-detect.html",
                .method = HTTP_GET,
                .handler = captive_portal_handler
            },
            {
                .uri = "/generate_204",
                .method = HTTP_GET,
                .handler = captive_portal_handler
            },
            {
                .uri = "/generate204",
                .method = HTTP_GET,
                .handler = captive_portal_handler
            },
            {
                .uri = "/api/config",
                .method = HTTP_GET,
                .handler = config_get_handler
            },
            {
                .uri = "/api/config",
                .method = HTTP_POST,
                .handler = config_post_handler
            },
            {
                .uri = "/api/scan",
                .method = HTTP_GET,
                .handler = scan_get_handler
            },
            {
                .uri = "/api/restart",
                .method = HTTP_GET,
                .handler = api_handler_restart
            },
            {
                .uri = "/api/reset",
                .method = HTTP_GET,
                .handler = api_handler_reset
            },
            {
                .uri = "/api/mqtt-client-id",
                .method = HTTP_GET,
                .handler = api_mqtt_client_id_get_handler
            },
            {
                .uri      = "/api/menjin/cmd",
                .method   = HTTP_GET,
                .handler  = api_handler_menjin_cmd,
            },
            {
                .uri = "/*",
                .method = HTTP_GET,
                .handler = rest_common_get_handler
            },
    };
    config.max_uri_handlers = sizeof(uri_handlers) / sizeof(uri_handlers[0]) + 2;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    for (int i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); ++i) {
        uri_handlers[i].user_ctx = rest_context;
        httpd_register_uri_handler(server, &uri_handlers[i]);
    }
    return ESP_OK;

err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}

esp_err_t stop_captive_portal()
{
    return httpd_stop(server);
}