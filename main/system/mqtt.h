//
// Created by Hessian on 2023/7/29.
//

#ifndef ESP_MENJIN_MQTT_H
#define ESP_MENJIN_MQTT_H
int generate_mqtt_client_id(char*);
char* mqtt_client_id();
void mqtt_task(void *pvParameters);
void mqtt_notify(char* content);

#endif //ESP_MENJIN_MQTT_H