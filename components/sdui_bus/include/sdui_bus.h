#ifndef SDUI_BUS_H
#define SDUI_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

// 定义总线订阅的回调函数签名
// 接收到的 payload 是纯文本（未深度解析的 JSON 字符串或纯字符串），由具体业务按需解析
typedef void (*sdui_bus_cb_t)(const char *payload);

// 初始化总线
void sdui_bus_init(void);

// 订阅云端下发的特定主题 (例如 "ui/update", "audio/play")
void sdui_bus_subscribe(const char *topic, sdui_bus_cb_t cb);

// 核心路由入口：仅供 websocket_manager 在收到下行文本时调用
void sdui_bus_route_down(const char *raw_json);

// 上行发布接口：各个模块调用此接口上报事件
// 总线会自动封装为 {"topic": "...", "payload": ...} 格式并发出
void sdui_bus_publish_up(const char *topic, const char *payload);

// 本地总线发布接口：在终端内部路由事件（不经过 WebSocket）
// 用于 Action URI "local://" 路由。触发对应 topic 的本地订阅者
void sdui_bus_publish_local(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif

#endif // SDUI_BUS_H