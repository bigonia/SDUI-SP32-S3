#ifndef TELEMETRY_MANAGER_H
#define TELEMETRY_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 设备遥测数据结构体
 * 所有字段在每次上报时采集，通过 sdui_bus 上行发送至服务器。
 */
typedef struct {
    char    device_id[18];          /**< 设备唯一码：WiFi MAC 地址（格式：AABBCCDDEEFF） */
    int     wifi_rssi;              /**< WiFi 信号强度（dBm），未连接时为 0 */
    char    ip[16];                 /**< 当前 IP 地址（点分十进制），未获取时为 "0.0.0.0" */
    float   temperature;            /**< 芯片内部温度（摄氏度），精度 ±5°C */
    uint32_t free_heap_internal;    /**< 内部 SRAM 剩余空间（字节） */
    uint32_t free_heap_total;       /**< 总空闲堆内存（含 PSRAM，字节） */
    uint64_t uptime_s;             /**< 系统运行时长（秒） */
} telemetry_data_t;

/**
 * @brief 启动遥测定时上报任务
 *
 * 创建一个后台 FreeRTOS 任务（栈分配在 PSRAM），以指定间隔采集设备状态
 * 并通过 sdui_bus 上行链路发送 "telemetry/heartbeat" 主题消息至服务器。
 *
 * @param report_interval_s 上报间隔（秒），建议 30~60 秒
 * @note 此函数应在 websocket_app_start() 之后调用，以确保上行链路已就绪
 */
void telemetry_app_start(uint32_t report_interval_s);

/**
 * @brief 获取设备唯一码（MAC 地址字符串）
 *
 * @param buf    输出缓冲区，至少 18 字节
 * @param len    缓冲区长度
 */
void telemetry_get_device_id(char *buf, size_t len);

/**
 * @brief 采集一次遥测快照（非上报，仅填充结构体）
 *
 * 可供其他模块按需调用，获取当前设备状态。
 *
 * @param data 输出的遥测数据结构体指针
 */
void telemetry_collect(telemetry_data_t *data);

#ifdef __cplusplus
}
#endif

#endif // TELEMETRY_MANAGER_H
