#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 初始化并启动 WiFi Station 模式
 * 内部包含了 NVS 挂载、事件循环注册及异步重连逻辑
 */
void wifi_init_sta(void);

/**
 * @brief 获取当前关联 AP 的 RSSI 信号强度
 * @return RSSI 值（dBm），未关联时返回 0
 */
int wifi_get_rssi(void);

/**
 * @brief 获取当前 IP 地址字符串
 * @param buf 输出缓冲区，至少 16 字节
 * @param len 缓冲区长度
 */
void wifi_get_ip_str(char *buf, size_t len);

/**
 * @brief 判断设备是否已保存 WiFi 配置信息
 * @return true 则已配网，false 则需要配网
 */
bool wifi_manager_is_provisioned(void);

/**
 * @brief 启动配网模式 (SoftAP + Web Server)
 */
void wifi_manager_start_provision(void);

/**
 * @brief 获取保存的 WebSocket 服务器地址
 * @param buf 输出缓冲区
 * @param len 缓冲区大小
 */
void wifi_manager_get_ws_url(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H