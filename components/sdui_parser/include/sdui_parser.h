/**
 * @file sdui_parser.h
 * @brief SDUI 容器化布局解析引擎
 *
 * 将 Server 下发的 JSON UI 描述树递归解析为 LVGL 对象。
 * 支持 container / label / button / image 等原子组件，
 * 以及 flex-box 布局、Action URI 事件绑定。
 *
 * 针对 1.75" 圆屏（466x466）定义了安全边距与居中约束。
 */
#ifndef SDUI_PARSER_H
#define SDUI_PARSER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 屏幕常量 ---- */
#define SDUI_SCREEN_W         466
#define SDUI_SCREEN_H         466
#define SDUI_SAFE_PADDING     40   // 圆屏安全内边距 (像素)

/**
 * @brief 初始化 SDUI 解析引擎
 * 
 * 在 LVGL 初始化之后、WebSocket 连接之前调用。
 * 创建一个带圆屏安全边距的根视图 (root_view)。
 *
 * @return 根视图对象指针
 */
lv_obj_t *sdui_parser_init(void);

/**
 * @brief 获取 SDUI 根视图
 * @return 根视图对象指针，若未初始化返回 NULL
 */
lv_obj_t *sdui_parser_get_root(void);

/**
 * @brief 根据完整的 JSON 布局描述重建 UI
 *
 * 此函数会：
 *   1. 清除根视图的所有子节点
 *   2. 根据 JSON 递归创建 LVGL 对象树
 *   3. 为带 action 字段的组件绑定事件回调
 *
 * @param json_str 完整的 JSON 布局字符串（来自 "ui/layout" 主题的 payload）
 *
 * @note 必须在 LVGL 加锁状态下调用 (bsp_display_lock)
 */
void sdui_parser_render(const char *json_str);

/**
 * @brief 根据 ID 查找已渲染的 LVGL 对象
 * @param id 组件 ID 字符串
 * @return 匹配的 lv_obj_t*，未找到返回 NULL
 */
lv_obj_t *sdui_parser_find_by_id(const char *id);

/**
 * @brief 根据 ID 更新指定组件的属性
 *
 * 支持增量更新（不重建整棵树），常用于 "ui/update" 主题。
 * payload 示例: {"id": "label_1", "text": "Hello"}
 *
 * @param json_str 增量更新 JSON 字符串
 * @note 必须在 LVGL 加锁状态下调用
 */
void sdui_parser_update(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif // SDUI_PARSER_H
