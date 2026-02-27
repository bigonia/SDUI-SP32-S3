#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "WS_MANAGER";

static esp_websocket_client_handle_t client = NULL;
static websocket_rx_cb_t global_rx_cb = NULL;
static bool is_connected = false;

// 大体积载荷拼接缓冲区
static char *rx_buffer = NULL;
static int rx_buffer_len = 0;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            is_connected = true;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            is_connected = false;
            // 清理可能未完成的接收缓冲
            if (rx_buffer) {
                heap_caps_free(rx_buffer);
                rx_buffer = NULL;
                rx_buffer_len = 0;
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            // 仅处理文本帧 (0x01) 或二进制流延续帧 (0x00)
            if (data->op_code == 0x01 || data->op_code == 0x00) {
                
                // 数据包起始标识
                if (data->payload_offset == 0) {
                    if (rx_buffer) {
                        heap_caps_free(rx_buffer);
                    }
                    // 为长数据分配连续的内部或外部 RAM
                    rx_buffer = (char *)heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_8BIT);
                    rx_buffer_len = 0;
                    if (!rx_buffer) {
                        ESP_LOGE(TAG, "No memory for RX buffer (size: %d)", data->payload_len);
                        return;
                    }
                }

                // 内存块拷贝拼接
                if (rx_buffer && (rx_buffer_len + data->data_len <= data->payload_len)) {
                    memcpy(rx_buffer + rx_buffer_len, data->data_ptr, data->data_len);
                    rx_buffer_len += data->data_len;
                }

                // 完整帧接收完毕
                if (rx_buffer_len == data->payload_len) {
                    rx_buffer[rx_buffer_len] = '\0'; // 字符串封尾
                    
                    if (global_rx_cb) {
                        global_rx_cb(rx_buffer); // 推入 SDUI 总线
                    }

                    // 用完即焚，释放内存池
                    heap_caps_free(rx_buffer);
                    rx_buffer = NULL;
                    rx_buffer_len = 0;
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
            break;
    }
}

void websocket_app_start(const char *uri, websocket_rx_cb_t cb)
{
    global_rx_cb = cb;

    const esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,   // 开启自动重连：断线后每隔 5 秒重试
        .network_timeout_ms = 10000,    // 物理网络超时判定阈值
        .buffer_size = 4096,            // 底层 TCP 接收缓冲区大小（增大，走 PSRAM）
    };

    ESP_LOGI(TAG, "Connecting to %s...", uri);
    client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);
}

void websocket_send_json(const char *payload)
{
    // 非阻塞拦截机制：物理断线时直接舍弃上行交互，避免任务死锁或看门狗复位
    if (!is_connected || client == NULL || payload == NULL) {
        ESP_LOGD(TAG, "Drop TX data: Websocket disconnected");
        return;
    }
    
    int len = strlen(payload);
    esp_websocket_client_send_text(client, payload, len, portMAX_DELAY);
}

void websocket_app_stop(void)
{
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
        is_connected = false;
    }
}