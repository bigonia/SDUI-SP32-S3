#ifndef WEBSOCKET_MANAGER_H
#define WEBSOCKET_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

// 定义底层的路由分发函数类型（当前由 sdui_bus 接管）
typedef void (*websocket_rx_cb_t)(const char *text);

/**
 * @brief 启动 WebSocket 守护进程
 * @param uri 目标服务器地址 (例如: ws://172.16.11.64:8080)
 * @param cb  数据接收完成后的路由回调 (通常传入 sdui_bus_route_down)
 */
void websocket_app_start(const char *uri, websocket_rx_cb_t cb);

/**
 * @brief 停止并销毁 WebSocket 客户端
 */
void websocket_app_stop(void);

/**
 * @brief 非阻塞式发送 JSON 数据
 * @param payload 待发送的 JSON 字符串
 * @note 若当前处于断线状态，数据将被直接丢弃以避免阻塞主线程
 */
void websocket_send_json(const char *payload);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_MANAGER_H