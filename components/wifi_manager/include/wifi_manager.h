#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动 WiFi Station 模式
 * 内部包含了 NVS 挂载、事件循环注册及异步重连逻辑
 */
void wifi_init_sta(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H