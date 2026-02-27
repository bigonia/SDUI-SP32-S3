/**
 * @file sdui_parser.h
 * @brief SDUI 容器化布局解析引擎 (增强版)
 *
 * 将 Server 下发的 JSON UI 描述树递归解析为 LVGL 对象。
 *
 * 支持组件类型:
 *   - container : Flex 容器，支持 scrollable 滚动属性
 *   - label     : 文本标签，支持 marquee 跑马灯 (long_mode)
 *   - button    : 可交互按钮，支持 on_click/on_press/on_release Action URI
 *   - image     : 流式图像，Base64(RGB565) 解码，存于 PSRAM，支持 spin 旋转动画
 *   - bar       : 进度指示条，支持 value/min/max/bg_color/indic_color
 *   - slider    : 滑动控制，支持 value/min/max/on_change 事件上报
 *   - particle  : 粒子特效，LVGL Canvas(PSRAM)，≤30 粒子
 *
 * 支持动画属性 (anim 字段，服务端驱动):
 *   - blink       : 透明度闪烁
 *   - breathe     : 透明度呼吸
 *   - spin        : 图片旋转 (限≤2个并发)
 *   - slide_in    : 方向滑入入场
 *   - shake       : 水平抖动
 *   - color_pulse : 背景色双色渐变
 *   - marquee     : label 跑马灯
 *
 * 针对 1.75" 圆屏 (466x466) 定义了安全边距与居中约束。
 */
#ifndef SDUI_PARSER_H
#define SDUI_PARSER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 屏幕常量 ---- */
#define SDUI_SCREEN_W     466
#define SDUI_SCREEN_H     466
#define SDUI_SAFE_PADDING  40   /* 圆屏安全内边距 (像素) */

/**
 * @brief 初始化 SDUI 解析引擎
 * 在 LVGL 初始化之后、WebSocket 连接之前调用。
 * @return 根视图对象指针
 */
lv_obj_t *sdui_parser_init(void);

/**
 * @brief 获取 SDUI 根视图
 * @return 根视图对象指针，未初始化返回 NULL
 */
lv_obj_t *sdui_parser_get_root(void);

/**
 * @brief 根据完整 JSON 布局描述重建 UI (含 Fade 过渡动画)
 *
 * 执行步骤：
 *   1. 根视图透明度瞬间降为 0 (隐藏旧内容)
 *   2. 清除所有子节点和 anim 动画
 *   3. 递归构建新 LVGL 对象树
 *   4. 启动 200ms Fade-In 动画
 *
 * @param json_str ui/layout 主题的 payload JSON 字符串
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
 * @brief 按 ID 增量更新组件属性
 *
 * 支持字段: text / hidden / bg_color / opa / value (bar/slider) /
 *           indic_color (bar) / anim (触发动画)
 *
 * @param json_str ui/update 主题的 payload JSON 字符串
 * @note 必须在 LVGL 加锁状态下调用
 */
void sdui_parser_update(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* SDUI_PARSER_H */
