/**
 * @file telemetry_manager.c
 * @brief 终端元数据定时上报模块
 *
 * 采集设备唯一码（eFuse MAC）、WiFi 信号、IP、芯片温度、堆内存余量及运行时长，
 * 通过 sdui_bus 上行链路定期发送 "telemetry/heartbeat" 主题到服务端。
 *
 * 性能考量：
 *   - 任务栈开在 PSRAM（4KB），不占宝贵内部 SRAM
 *   - 上报周期默认 30s，JSON payload ≈ 160 字节，网络负担极低
 *   - 仅在 WebSocket 已连接时上报，断线跳过，不堆积
 */

#include "telemetry_manager.h"
#include "sdui_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "driver/temperature_sensor.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "TELEMETRY";

/* ---- 模块静态状态 ---- */
static char s_device_id[18]   = {0};   // 缓存设备 ID，只读取一次
static uint32_t s_interval_s  = 30;    // 上报间隔（秒）
static temperature_sensor_handle_t s_temp_sensor = NULL;

/* ---- 内部：初始化温度传感器 ---- */
static void prv_init_temp_sensor(void)
{
    temperature_sensor_config_t temp_sensor_config = {
        .range_min = 20,
        .range_max = 100,
    };
    esp_err_t err = temperature_sensor_install(&temp_sensor_config, &s_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Temperature sensor install failed: %s", esp_err_to_name(err));
        s_temp_sensor = NULL;
        return;
    }
    temperature_sensor_enable(s_temp_sensor);
    ESP_LOGI(TAG, "Temperature sensor initialized");
}

/* ---- 内部：一次性读取并缓存 MAC 地址作为设备 ID ---- */
static void prv_init_device_id(void)
{
    if (s_device_id[0] != '\0') return; // 已初始化，跳过

    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id),
                 "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strncpy(s_device_id, "UNKNOWN", sizeof(s_device_id) - 1);
        ESP_LOGW(TAG, "Failed to read eFuse MAC: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);

    // 将设备 ID 注册到总线，之后所有上行消息将自动携带 device_id
    sdui_bus_set_device_id(s_device_id);
}

/* ---- 采集遥测快照 ---- */
void telemetry_collect(telemetry_data_t *data)
{
    if (!data) return;

    // 1. 设备唯一码（已缓存，零开销）
    strncpy(data->device_id, s_device_id, sizeof(data->device_id) - 1);
    data->device_id[sizeof(data->device_id) - 1] = '\0';

    // 2. WiFi 信号强度（RSSI）
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        data->wifi_rssi = ap_info.rssi;
    } else {
        data->wifi_rssi = 0;
    }

    // 3. IP 地址
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(data->ip, sizeof(data->ip), IPSTR, IP2STR(&ip_info.ip));
        } else {
            strncpy(data->ip, "0.0.0.0", sizeof(data->ip) - 1);
        }
    } else {
        strncpy(data->ip, "0.0.0.0", sizeof(data->ip) - 1);
    }

    // 4. 芯片内部温度
    if (s_temp_sensor) {
        float tsens_value = 0.0f;
        if (temperature_sensor_get_celsius(s_temp_sensor, &tsens_value) == ESP_OK) {
            data->temperature = tsens_value;
        } else {
            data->temperature = -1.0f;
        }
    } else {
        data->temperature = -1.0f;
    }

    // 5. 内存状态
    data->free_heap_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    data->free_heap_total    = (uint32_t)esp_get_free_heap_size();

    // 6. 运行时长（esp_timer 返回微秒，转换为秒）
    data->uptime_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);
}

/* ---- 获取设备 ID（供外部模块调用） ---- */
void telemetry_get_device_id(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_device_id, len - 1);
    buf[len - 1] = '\0';
}

/* ---- 上报任务：序列化为 JSON 并通过总线上行 ---- */
static void telemetry_report_task(void *arg)
{
    ESP_LOGI(TAG, "Telemetry task started, interval=%lus", (unsigned long)s_interval_s);

    // 首次上报前等待 WebSocket 连接建立（延迟 5 秒）
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        // --- 采集快照 ---
        telemetry_data_t data = {0};
        telemetry_collect(&data);

        // --- 序列化为 JSON ---
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "device_id",          data.device_id);
            cJSON_AddNumberToObject(root, "wifi_rssi",          data.wifi_rssi);
            cJSON_AddStringToObject(root, "ip",                 data.ip);
            cJSON_AddNumberToObject(root, "temperature",        (double)data.temperature);
            cJSON_AddNumberToObject(root, "free_heap_internal", (double)data.free_heap_internal);
            cJSON_AddNumberToObject(root, "free_heap_total",    (double)data.free_heap_total);
            cJSON_AddNumberToObject(root, "uptime_s",           (double)data.uptime_s);

            char *json_str = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            if (json_str) {
                ESP_LOGI(TAG, "Reporting: id=%s rssi=%d ip=%s temp=%.1f heap_int=%lu",
                         data.device_id, data.wifi_rssi, data.ip,
                         data.temperature, (unsigned long)data.free_heap_internal);

                // --- 通过 SDUI Bus 上行 ---
                sdui_bus_publish_up("telemetry/heartbeat", json_str);
                free(json_str);
            }
        }

        // --- 等待下次上报周期 ---
        vTaskDelay(pdMS_TO_TICKS(s_interval_s * 1000UL));
    }
}

/* ---- 对外接口：启动遥测模块 ---- */
void telemetry_app_start(uint32_t report_interval_s)
{
    // 使用 printf 确保在任何 log 级别下都可见
    printf("[TELEMETRY] telemetry_app_start() called, interval=%lu s\n",
           (unsigned long)report_interval_s);

    s_interval_s = (report_interval_s > 0) ? report_interval_s : 30;

    // 初始化设备 ID 和温度传感器（仅一次）
    prv_init_device_id();
    prv_init_temp_sensor();

    // 创建后台上报任务，栈开在 PSRAM，避免占用内部 SRAM
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        telemetry_report_task,
        "telemetry_task",
        4096,           // 4KB 栈，cJSON 序列化足够
        NULL,
        2,              // 低优先级，低于音频/UI 任务
        NULL,
        1,              // Core 1（与音频任务同核，但优先级更低）
        MALLOC_CAP_SPIRAM
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create telemetry task! ret=%d", (int)ret);
        printf("[TELEMETRY] ERROR: Task creation failed! ret=%d\n", (int)ret);
    } else {
        ESP_LOGE(TAG, "Telemetry manager STARTED OK (interval=%lus)",
                 (unsigned long)s_interval_s);
        printf("[TELEMETRY] Task created OK\n");
    }
}
