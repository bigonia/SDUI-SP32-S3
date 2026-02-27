/**
 * @file sdui_parser.c
 * @brief SDUI 容器化布局解析引擎实现
 *
 * 将 Server 下发的 JSON UI 树递归解析为 LVGL 对象。
 * 支持组件类型: container, label, button, image
 * 支持布局: flex-box (row/column), 对齐方式, 尺寸百分比/像素
 * 支持事件: on_click, on_press, on_release → Action URI (local:// / server://)
 */
#include "sdui_parser.h"
#include "sdui_bus.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "SDUI_PARSER";

/* ---- 根视图 ---- */
static lv_obj_t *s_root_view = NULL;

/* ---- ID 注册表 (用于按 ID 查找组件) ---- */
#define MAX_ID_ENTRIES 32
typedef struct {
    char id[32];
    lv_obj_t *obj;
} id_entry_t;

static id_entry_t s_id_table[MAX_ID_ENTRIES];
static int s_id_count = 0;

/* ---- Action URI 用户数据 (挂载到 LVGL 对象) ---- */
typedef struct {
    char on_click[64];
    char on_press[64];
    char on_release[64];
} action_data_t;

/* --------- 前向声明 --------- */
static void parse_node(cJSON *node, lv_obj_t *parent);
static void apply_common_style(cJSON *node, lv_obj_t *obj);
static void register_id(const char *id, lv_obj_t *obj);
static void action_event_cb(lv_event_t *e);
static void dispatch_action(const char *action_uri, const char *widget_id);

/* ---- 工具函数: 解析颜色 ---- */
static lv_color_t parse_color(const char *hex_str) {
    if (!hex_str || hex_str[0] != '#' || strlen(hex_str) < 7) {
        return lv_color_white();
    }
    uint32_t color_val = (uint32_t)strtol(hex_str + 1, NULL, 16);
    return lv_color_hex(color_val);
}

/* ---- 工具函数: 解析对齐方式 ---- */
static lv_align_t parse_align(const char *align_str) {
    if (!align_str) return LV_ALIGN_DEFAULT;
    if (strcmp(align_str, "center") == 0)       return LV_ALIGN_CENTER;
    if (strcmp(align_str, "top_mid") == 0)      return LV_ALIGN_TOP_MID;
    if (strcmp(align_str, "top_left") == 0)     return LV_ALIGN_TOP_LEFT;
    if (strcmp(align_str, "top_right") == 0)    return LV_ALIGN_TOP_RIGHT;
    if (strcmp(align_str, "bottom_mid") == 0)   return LV_ALIGN_BOTTOM_MID;
    if (strcmp(align_str, "bottom_left") == 0)  return LV_ALIGN_BOTTOM_LEFT;
    if (strcmp(align_str, "bottom_right") == 0) return LV_ALIGN_BOTTOM_RIGHT;
    if (strcmp(align_str, "left_mid") == 0)     return LV_ALIGN_LEFT_MID;
    if (strcmp(align_str, "right_mid") == 0)    return LV_ALIGN_RIGHT_MID;
    return LV_ALIGN_DEFAULT;
}

/* ---- 工具函数: 解析 Flex 方向 ---- */
static lv_flex_flow_t parse_flex_flow(const char *flow_str) {
    if (!flow_str) return LV_FLEX_FLOW_COLUMN;
    if (strcmp(flow_str, "row") == 0)          return LV_FLEX_FLOW_ROW;
    if (strcmp(flow_str, "column") == 0)       return LV_FLEX_FLOW_COLUMN;
    if (strcmp(flow_str, "row_wrap") == 0)     return LV_FLEX_FLOW_ROW_WRAP;
    if (strcmp(flow_str, "column_wrap") == 0)  return LV_FLEX_FLOW_COLUMN_WRAP;
    return LV_FLEX_FLOW_COLUMN;
}

/* ---- 工具函数: 解析 Flex 对齐 ---- */
static lv_flex_align_t parse_flex_align(const char *align_str) {
    if (!align_str) return LV_FLEX_ALIGN_START;
    if (strcmp(align_str, "start") == 0)          return LV_FLEX_ALIGN_START;
    if (strcmp(align_str, "end") == 0)            return LV_FLEX_ALIGN_END;
    if (strcmp(align_str, "center") == 0)         return LV_FLEX_ALIGN_CENTER;
    if (strcmp(align_str, "space_evenly") == 0)   return LV_FLEX_ALIGN_SPACE_EVENLY;
    if (strcmp(align_str, "space_around") == 0)   return LV_FLEX_ALIGN_SPACE_AROUND;
    if (strcmp(align_str, "space_between") == 0)  return LV_FLEX_ALIGN_SPACE_BETWEEN;
    return LV_FLEX_ALIGN_START;
}

/* ======== 解析尺寸值 (支持百分比 "50%" 和像素 100) ======== */
static lv_coord_t parse_size_value(cJSON *item) {
    if (!item) return LV_SIZE_CONTENT;
    if (cJSON_IsNumber(item)) {
        return (lv_coord_t)item->valueint;
    }
    if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        if (strcmp(s, "full") == 0) return lv_pct(100);
        if (strcmp(s, "content") == 0) return LV_SIZE_CONTENT;
        // 百分比格式 "50%"
        int len = strlen(s);
        if (len > 1 && s[len - 1] == '%') {
            int pct = atoi(s);
            return lv_pct(pct);
        }
        return (lv_coord_t)atoi(s);
    }
    return LV_SIZE_CONTENT;
}

/* ======== 公共样式应用 ======== */
static void apply_common_style(cJSON *node, lv_obj_t *obj) {
    /* 尺寸 */
    cJSON *w = cJSON_GetObjectItem(node, "w");
    cJSON *h = cJSON_GetObjectItem(node, "h");
    if (w) lv_obj_set_width(obj, parse_size_value(w));
    if (h) lv_obj_set_height(obj, parse_size_value(h));

    /* 对齐 */
    cJSON *align = cJSON_GetObjectItem(node, "align");
    cJSON *x_ofs = cJSON_GetObjectItem(node, "x");
    cJSON *y_ofs = cJSON_GetObjectItem(node, "y");
    if (align && cJSON_IsString(align)) {
        int xo = x_ofs ? x_ofs->valueint : 0;
        int yo = y_ofs ? y_ofs->valueint : 0;
        lv_obj_align(obj, parse_align(align->valuestring), xo, yo);
    }

    /* 背景色 */
    cJSON *bg_color = cJSON_GetObjectItem(node, "bg_color");
    if (bg_color && cJSON_IsString(bg_color)) {
        lv_obj_set_style_bg_color(obj, parse_color(bg_color->valuestring), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }

    /* 背景透明度 */
    cJSON *bg_opa = cJSON_GetObjectItem(node, "bg_opa");
    if (bg_opa && cJSON_IsNumber(bg_opa)) {
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)bg_opa->valueint, 0);
    }

    /* 内边距 */
    cJSON *pad = cJSON_GetObjectItem(node, "pad");
    if (pad && cJSON_IsNumber(pad)) {
        lv_obj_set_style_pad_all(obj, (lv_coord_t)pad->valueint, 0);
    }

    /* 圆角 */
    cJSON *radius = cJSON_GetObjectItem(node, "radius");
    if (radius && cJSON_IsNumber(radius)) {
        lv_obj_set_style_radius(obj, (lv_coord_t)radius->valueint, 0);
    }

    /* 间距 (Flex gap) */
    cJSON *gap = cJSON_GetObjectItem(node, "gap");
    if (gap && cJSON_IsNumber(gap)) {
        lv_obj_set_style_pad_row(obj, (lv_coord_t)gap->valueint, 0);
        lv_obj_set_style_pad_column(obj, (lv_coord_t)gap->valueint, 0);
    }

    /* 边框 */
    cJSON *border_w = cJSON_GetObjectItem(node, "border_w");
    if (border_w && cJSON_IsNumber(border_w)) {
        lv_obj_set_style_border_width(obj, (lv_coord_t)border_w->valueint, 0);
    }
    cJSON *border_color = cJSON_GetObjectItem(node, "border_color");
    if (border_color && cJSON_IsString(border_color)) {
        lv_obj_set_style_border_color(obj, parse_color(border_color->valuestring), 0);
    }

    /* 文字颜色 (用于 label/button 内 label) */
    cJSON *text_color = cJSON_GetObjectItem(node, "text_color");
    if (text_color && cJSON_IsString(text_color)) {
        lv_obj_set_style_text_color(obj, parse_color(text_color->valuestring), 0);
    }

    /* 文字大小 */
    cJSON *font_size = cJSON_GetObjectItem(node, "font_size");
    if (font_size && cJSON_IsNumber(font_size)) {
        int sz = font_size->valueint;
        /* 映射到 LVGL 内嵌 Montserrat 字体 */
        const lv_font_t *font = &lv_font_montserrat_14;
        if (sz >= 26) font = &lv_font_montserrat_26;
        else if (sz >= 24) font = &lv_font_montserrat_24;
        else if (sz >= 20) font = &lv_font_montserrat_20;
        else if (sz >= 16) font = &lv_font_montserrat_16;
        lv_obj_set_style_text_font(obj, font, 0);
    }

    /* 隐藏/显示 */
    cJSON *hidden = cJSON_GetObjectItem(node, "hidden");
    if (hidden && cJSON_IsTrue(hidden)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ======== Action URI 事件回调 (统一入口) ======== */
static void action_event_cb(lv_event_t *e)
{
    action_data_t *ad = (action_data_t *)lv_event_get_user_data(e);
    if (!ad) return;

    lv_event_code_t code = lv_event_get_code(e);

    /* 查找该组件的 ID (遍历注册表) */
    const char *widget_id = "unknown";
    lv_obj_t *target = lv_event_get_target(e);
    for (int i = 0; i < s_id_count; i++) {
        if (s_id_table[i].obj == target) {
            widget_id = s_id_table[i].id;
            break;
        }
    }

    if (code == LV_EVENT_CLICKED && ad->on_click[0] != '\0') {
        dispatch_action(ad->on_click, widget_id);
    } else if (code == LV_EVENT_PRESSED && ad->on_press[0] != '\0') {
        dispatch_action(ad->on_press, widget_id);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && ad->on_release[0] != '\0') {
        dispatch_action(ad->on_release, widget_id);
    }
}

/* ======== Action URI 分发 ======== */
static void dispatch_action(const char *action_uri, const char *widget_id)
{
    if (!action_uri || action_uri[0] == '\0') return;

    ESP_LOGI(TAG, "Action dispatch: uri=%s, widget=%s", action_uri, widget_id);

    if (strncmp(action_uri, "local://", 8) == 0) {
        /* 本地总线路由: local://audio/cmd/record_start → topic = "audio/cmd/record_start" */
        const char *topic = action_uri + 8;
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"id\": \"%s\"}", widget_id);
        sdui_bus_publish_local(topic, payload);
    } else if (strncmp(action_uri, "server://", 9) == 0) {
        /* 显式上报云端 */
        const char *topic = action_uri + 9;
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"id\": \"%s\"}", widget_id);
        sdui_bus_publish_up(topic, payload);
    } else {
        /* 默认行为: 上报 ui/click (兼容旧协议) */
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"id\": \"%s\"}", widget_id);
        sdui_bus_publish_up("ui/click", payload);
    }
}

/* ======== 绑定 Action URI 到 LVGL 对象 ======== */
static void bind_actions(cJSON *node, lv_obj_t *obj)
{
    cJSON *on_click   = cJSON_GetObjectItem(node, "on_click");
    cJSON *on_press   = cJSON_GetObjectItem(node, "on_press");
    cJSON *on_release = cJSON_GetObjectItem(node, "on_release");

    if (!on_click && !on_press && !on_release) return;

    /* 使用 heap 分配 action_data，由 LVGL 对象生命周期管理 */
    action_data_t *ad = (action_data_t *)calloc(1, sizeof(action_data_t));
    if (!ad) {
        ESP_LOGE(TAG, "Failed to alloc action_data");
        return;
    }

    if (on_click && cJSON_IsString(on_click)) {
        strncpy(ad->on_click, on_click->valuestring, sizeof(ad->on_click) - 1);
    }
    if (on_press && cJSON_IsString(on_press)) {
        strncpy(ad->on_press, on_press->valuestring, sizeof(ad->on_press) - 1);
    }
    if (on_release && cJSON_IsString(on_release)) {
        strncpy(ad->on_release, on_release->valuestring, sizeof(ad->on_release) - 1);
    }

    /* 注册 LVGL 事件：使用 LV_EVENT_ALL 以捕获 CLICKED/PRESSED/RELEASED */
    lv_obj_add_event_cb(obj, action_event_cb, LV_EVENT_ALL, ad);
}

/* ======== ID 注册 ======== */
static void register_id(const char *id, lv_obj_t *obj) {
    if (s_id_count >= MAX_ID_ENTRIES) {
        ESP_LOGW(TAG, "ID table full, cannot register: %s", id);
        return;
    }
    strncpy(s_id_table[s_id_count].id, id, sizeof(s_id_table[s_id_count].id) - 1);
    s_id_table[s_id_count].id[sizeof(s_id_table[s_id_count].id) - 1] = '\0';
    s_id_table[s_id_count].obj = obj;
    s_id_count++;
}

/* ======== 清空 ID 注册表 ======== */
static void clear_id_table(void) {
    s_id_count = 0;
    memset(s_id_table, 0, sizeof(s_id_table));
}

/* ======== 释放挂载在 LVGL 对象上的 action_data ======== */
static void free_action_data_cb(lv_event_t *e) {
    action_data_t *ad = (action_data_t *)lv_event_get_user_data(e);
    if (ad) {
        free(ad);
    }
}

/* ======== 创建 container 类型 ======== */
static lv_obj_t *create_container(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);

    /* Flex 布局 */
    cJSON *flex = cJSON_GetObjectItem(node, "flex");
    if (flex && cJSON_IsString(flex)) {
        lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cont, parse_flex_flow(flex->valuestring));
    }

    /* Flex 对齐 */
    cJSON *justify = cJSON_GetObjectItem(node, "justify");
    cJSON *align_items = cJSON_GetObjectItem(node, "align_items");
    if (justify || align_items) {
        lv_flex_align_t main_align = justify && cJSON_IsString(justify) ?
            parse_flex_align(justify->valuestring) : LV_FLEX_ALIGN_START;
        lv_flex_align_t cross_align = align_items && cJSON_IsString(align_items) ?
            parse_flex_align(align_items->valuestring) : LV_FLEX_ALIGN_START;
        lv_obj_set_flex_align(cont, main_align, cross_align, cross_align);
    }

    /* 默认尺寸为内容自适应 */
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* 默认去除 scrollbar */
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    return cont;
}

/* ======== 创建 label 类型 ======== */
static lv_obj_t *create_label(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);

    cJSON *text = cJSON_GetObjectItem(node, "text");
    lv_label_set_text(label, (text && cJSON_IsString(text)) ? text->valuestring : "");

    /* 长文本处理 */
    cJSON *long_mode = cJSON_GetObjectItem(node, "long_mode");
    if (long_mode && cJSON_IsString(long_mode)) {
        if (strcmp(long_mode->valuestring, "wrap") == 0) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        } else if (strcmp(long_mode->valuestring, "scroll") == 0) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL);
        } else if (strcmp(long_mode->valuestring, "dot") == 0) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        }
    }

    return label;
}

/* ======== 创建 button 类型 ======== */
static lv_obj_t *create_button(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* 按钮内嵌 label */
    cJSON *text = cJSON_GetObjectItem(node, "text");
    if (text && cJSON_IsString(text)) {
        lv_obj_t *btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, text->valuestring);
        lv_obj_center(btn_label);

        /* 按钮内的文字颜色 */
        cJSON *text_color = cJSON_GetObjectItem(node, "text_color");
        if (text_color && cJSON_IsString(text_color)) {
            lv_obj_set_style_text_color(btn_label, parse_color(text_color->valuestring), 0);
        }
    }

    return btn;
}

/* ======== 递归解析节点 ======== */
static void parse_node(cJSON *node, lv_obj_t *parent) {
    if (!node || !parent) return;

    cJSON *type = cJSON_GetObjectItem(node, "type");
    if (!type || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Node missing 'type' field, skipping");
        return;
    }

    lv_obj_t *obj = NULL;
    const char *type_str = type->valuestring;

    /* ---- 原子组件创建 ---- */
    if (strcmp(type_str, "container") == 0) {
        obj = create_container(node, parent);
    } else if (strcmp(type_str, "label") == 0) {
        obj = create_label(node, parent);
    } else if (strcmp(type_str, "button") == 0) {
        obj = create_button(node, parent);
    } else {
        ESP_LOGW(TAG, "Unknown widget type: %s", type_str);
        return;
    }

    if (!obj) return;

    /* 注册 ID */
    cJSON *id = cJSON_GetObjectItem(node, "id");
    if (id && cJSON_IsString(id)) {
        register_id(id->valuestring, obj);
    }

    /* 应用通用样式 */
    apply_common_style(node, obj);

    /* 绑定 Action URI */
    bind_actions(node, obj);

    /* 注册删除回调以释放 action_data */
    lv_obj_add_event_cb(obj, free_action_data_cb, LV_EVENT_DELETE, NULL);

    /* 递归子节点 */
    cJSON *children = cJSON_GetObjectItem(node, "children");
    if (children && cJSON_IsArray(children)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, children) {
            parse_node(child, obj);
        }
    }
}

/* ======== 公共 API ======== */

lv_obj_t *sdui_parser_init(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 创建根视图：带圆屏安全边距 */
    s_root_view = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root_view);
    lv_obj_set_size(s_root_view,
                    SDUI_SCREEN_W - 2 * SDUI_SAFE_PADDING,
                    SDUI_SCREEN_H - 2 * SDUI_SAFE_PADDING);
    lv_obj_center(s_root_view);
    lv_obj_set_style_bg_opa(s_root_view, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root_view, LV_OBJ_FLAG_SCROLLABLE);

    /* 根视图默认启用 Flex Column 布局 */
    lv_obj_set_layout(s_root_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_root_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root_view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    clear_id_table();

    ESP_LOGI(TAG, "SDUI Parser initialized. Root view: %dx%d (safe padding: %d)",
             SDUI_SCREEN_W - 2 * SDUI_SAFE_PADDING,
             SDUI_SCREEN_H - 2 * SDUI_SAFE_PADDING,
             SDUI_SAFE_PADDING);

    return s_root_view;
}

lv_obj_t *sdui_parser_get_root(void)
{
    return s_root_view;
}

void sdui_parser_render(const char *json_str)
{
    if (!json_str || !s_root_view) return;

    ESP_LOGI(TAG, "Rendering layout from JSON (%d bytes)", strlen(json_str));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse layout JSON");
        return;
    }

    /* 清除现有 UI 树 */
    lv_obj_clean(s_root_view);
    clear_id_table();

    /* 解析根级别：如果是数组，逐个解析；如果是对象，解析单个节点 */
    if (cJSON_IsArray(root)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) {
            parse_node(item, s_root_view);
        }
    } else if (cJSON_IsObject(root)) {
        /* 单个根对象：检查是否有 children 数组 */
        cJSON *children = cJSON_GetObjectItem(root, "children");
        if (children && cJSON_IsArray(children)) {
            /* 先解析根节点自身的属性到 root_view */
            apply_common_style(root, s_root_view);

            /* Flex 布局 */
            cJSON *flex = cJSON_GetObjectItem(root, "flex");
            if (flex && cJSON_IsString(flex)) {
                lv_obj_set_layout(s_root_view, LV_LAYOUT_FLEX);
                lv_obj_set_flex_flow(s_root_view, parse_flex_flow(flex->valuestring));
            }

            cJSON *child = NULL;
            cJSON_ArrayForEach(child, children) {
                parse_node(child, s_root_view);
            }
        } else {
            /* 如果自身带 type，作为单个节点解析 */
            parse_node(root, s_root_view);
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Layout render complete. Registered %d IDs.", s_id_count);
}

lv_obj_t *sdui_parser_find_by_id(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_id_count; i++) {
        if (strcmp(s_id_table[i].id, id) == 0) {
            return s_id_table[i].obj;
        }
    }
    return NULL;
}

void sdui_parser_update(const char *json_str)
{
    if (!json_str) return;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse update JSON");
        return;
    }

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (!id_item || !cJSON_IsString(id_item)) {
        ESP_LOGW(TAG, "Update JSON missing 'id' field");
        cJSON_Delete(root);
        return;
    }

    lv_obj_t *target = sdui_parser_find_by_id(id_item->valuestring);
    if (!target) {
        ESP_LOGW(TAG, "Widget not found for update: %s", id_item->valuestring);
        cJSON_Delete(root);
        return;
    }

    /* 更新文本内容 */
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && cJSON_IsString(text)) {
        /* 判断类型：如果是 label 则直接设置，如果是 btn 则找子 label */
        lv_obj_t *label_obj = target;
        if (lv_obj_get_child_count(target) > 0) {
            /* Button 的第一个子组件通常是内嵌 label */
            label_obj = lv_obj_get_child(target, 0);
        }
        lv_label_set_text(label_obj, text->valuestring);
    }

    /* 更新可见性 */
    cJSON *hidden = cJSON_GetObjectItem(root, "hidden");
    if (hidden) {
        if (cJSON_IsTrue(hidden)) {
            lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 更新背景色 */
    cJSON *bg_color = cJSON_GetObjectItem(root, "bg_color");
    if (bg_color && cJSON_IsString(bg_color)) {
        lv_obj_set_style_bg_color(target, parse_color(bg_color->valuestring), 0);
        lv_obj_set_style_bg_opa(target, LV_OPA_COVER, 0);
    }

    ESP_LOGI(TAG, "Updated widget: %s", id_item->valuestring);
    cJSON_Delete(root);
}
