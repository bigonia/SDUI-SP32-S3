#include "imu_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include "sdui_bus.h"

// 消除宏定义冲突警告
#undef M_PI 

// 引入真实的头文件
#include "qmi8658.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "IMU_MANAGER";

static void imu_polling_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Hardware IMU task starting...");

    // 1. 完全采用你附件中的官方初始化方式
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    qmi8658_dev_t *dev = malloc(sizeof(qmi8658_dev_t));
    
    // 初始化并配置量程和刷新率
    if (qmi8658_init(dev, bus_handle, QMI8658_ADDRESS_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMI8658!");
        vTaskDelete(NULL);
    }
    qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(dev, true); // 开启后，数据的单位是 m/s² (1G ≈ 9.8)
    qmi8658_write_register(dev, QMI8658_CTRL5, 0x03);

    ESP_LOGI(TAG, "QMI8658 Initialized. Waiting for shake events...");

    int shake_cooldown = 0;
    qmi8658_data_t data;

    while (1) {
        bool ready;
        // 2. 检查数据是否准备好
        if (qmi8658_is_data_ready(dev, &ready) == ESP_OK && ready) {
            // 读取真实的三轴数据
            if (qmi8658_read_sensor_data(dev, &data) == ESP_OK) {
                
                // 计算三维空间加速度模长 (单位：m/s²)
                float acc_magnitude = sqrt(data.accelX * data.accelX + 
                                           data.accelY * data.accelY + 
                                           data.accelZ * data.accelZ);

                if (shake_cooldown > 0) {
                    shake_cooldown--;
                }

                // 判定阈值：由于单位是 m/s²，1.5G 相当于 1.5 * 9.8 ≈ 14.7 m/s²
                if (acc_magnitude > 14.7f && shake_cooldown == 0) {
                    ESP_LOGI(TAG, "Real Hardware Shake detected! Magnitude: %.2f m/s²", acc_magnitude);
                    
                    char json_payload[128];
                    snprintf(json_payload, sizeof(json_payload), 
                             "{\"type\": \"shake\", \"magnitude\": %.2f}", 
                             acc_magnitude);
                    
                    // 通过消息总线上行发布，解耦 WebSocket 依赖
                    sdui_bus_publish_up("motion", json_payload);
                    
                    shake_cooldown = 10; // 10 次轮询冷却（约 1 秒）避免重复触发
                }
            }
        }
        // 10Hz 轮询 (100ms)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void imu_app_start(void)
{
    xTaskCreate(imu_polling_task, "imu_polling_task", 4096, NULL, 5, NULL);
}