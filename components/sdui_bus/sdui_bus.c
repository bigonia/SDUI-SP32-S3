#include "sdui_bus.h"
#include "websocket_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

#define MAX_SUBSCRIBERS 15
static const char *TAG = "SDUI_BUS";

// 订阅者注册表节点
typedef struct {
    char topic[32];
    sdui_bus_cb_t cb;
} sdui_subscriber_t;

static sdui_subscriber_t subscribers[MAX_SUBSCRIBERS];
static uint8_t sub_count = 0;

void sdui_bus_init(void) {
    sub_count = 0;
    ESP_LOGI(TAG, "SDUI Bus Initialized");
}

void sdui_bus_subscribe(const char *topic, sdui_bus_cb_t cb) {
    if (sub_count < MAX_SUBSCRIBERS) {
        strncpy(subscribers[sub_count].topic, topic, sizeof(subscribers[sub_count].topic) - 1);
        subscribers[sub_count].topic[sizeof(subscribers[sub_count].topic) - 1] = '\0';
        subscribers[sub_count].cb = cb;
        sub_count++;
        ESP_LOGI(TAG, "Subscribed to topic: %s", topic);
    } else {
        ESP_LOGE(TAG, "Failed to subscribe %s: Max subscribers reached!", topic);
    }
}

void sdui_bus_route_down(const char *raw_json) {
    // 第一层浅解析，仅拆包封套
    cJSON *root = cJSON_Parse(raw_json);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse incoming SDUI payload");
        return;
    }

    cJSON *topic_item = cJSON_GetObjectItem(root, "topic");
    cJSON *payload_item = cJSON_GetObjectItem(root, "payload");

    if (topic_item && cJSON_IsString(topic_item)) {
        const char *topic = topic_item->valuestring;
        char *payload_str = NULL;

        if (payload_item) {
            // 将 payload 提取为字符串，交由下游业务自行处理
            if (cJSON_IsString(payload_item)) {
                payload_str = strdup(payload_item->valuestring);
            } else {
                payload_str = cJSON_PrintUnformatted(payload_item);
            }
        }

        // 路由分发机制
        for (int i = 0; i < sub_count; i++) {
            if (strcmp(subscribers[i].topic, topic) == 0) {
                if (subscribers[i].cb) {
                    subscribers[i].cb(payload_str);
                }
            }
        }
        
        if (payload_str) free(payload_str);
    }
    cJSON_Delete(root);
}

void sdui_bus_publish_up(const char *topic, const char *payload) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "topic", topic);
    
    // 尝试判断 payload 是否为有效 JSON 以保持结构扁平化
    cJSON *payload_json = cJSON_Parse(payload);
    if (payload_json) {
        cJSON_AddItemToObject(root, "payload", payload_json);
    } else {
        cJSON_AddStringToObject(root, "payload", payload ? payload : "");
    }

    char *out_str = cJSON_PrintUnformatted(root);
    if (out_str) {
        websocket_send_json(out_str); // 调用底层 websocket_manager 发送
        free(out_str);
    }
    cJSON_Delete(root);
}

void sdui_bus_publish_local(const char *topic, const char *payload) {
    if (!topic) return;
    ESP_LOGI(TAG, "Local publish: topic=%s", topic);

    for (int i = 0; i < sub_count; i++) {
        if (strcmp(subscribers[i].topic, topic) == 0) {
            if (subscribers[i].cb) {
                subscribers[i].cb(payload);
            }
        }
    }
}