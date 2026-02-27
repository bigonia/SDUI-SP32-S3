/**
 * @file sdui_parser.c
 * @brief SDUI 容器化布局解析引擎实现 (增强版)
 *
 * 将 Server 下发的 JSON UI 树递归解析为 LVGL 对象。
 * 支持组件类型: container, label, button, image, bar, slider, particle
 * 支持布局: flex-box (row/column), 对齐方式, 尺寸百分比/像素
 * 支持事件: on_click, on_press, on_release, on_change → Action URI
 * 支持动画: anim 字段 (blink/breathe/spin/slide_in/shake/color_pulse/marquee)
 * 支持特效: 页面切换 Fade 过渡, 粒子系统 (PSRAM Canvas)
 */
#include "sdui_parser.h"
#include "sdui_bus.h"
#include "audio_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "SDUI_PARSER";

/* ---- 根视图 ---- */
static lv_obj_t *s_root_view = NULL;

/* ---- ID 注册表 ---- */
#define MAX_ID_ENTRIES 64
typedef struct {
    char id[32];
    lv_obj_t *obj;
} id_entry_t;
static id_entry_t s_id_table[MAX_ID_ENTRIES];
static int        s_id_count = 0;

/* ---- 旋转动画计数（防止同时旋转超过 MAX_SPIN_ANIM 个） ---- */
#define MAX_SPIN_ANIM 2
static int s_spin_count = 0;

/* -------- 数据结构 -------- */

/** Action URI 用户数据，挂载到交互组件 */
typedef struct {
    char on_click[64];
    char on_press[64];
    char on_release[64];
} action_data_t;

/** image 组件：持有解码后的图像缓冲 */
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t       *data_buf; /* heap_caps_malloc(PSRAM) */
} image_data_t;

/** slider on_change 用户数据 */
typedef struct {
    char on_change[64];
    char id[32];
} slider_data_t;

/** color_pulse 动画目标颜色对 */
typedef struct {
    lv_obj_t  *obj;
    lv_color_t color_a;
    lv_color_t color_b;
} color_anim_data_t;

/** 粒子系统数据 */
#define MAX_PARTICLES 30
typedef struct {
    float   x, y;
    float   vx, vy;
    uint8_t alpha;
    bool    active;
} particle_t;

typedef struct {
    lv_obj_t     *canvas;
    particle_t    p[MAX_PARTICLES];
    int           count;
    lv_color_t    color;
    int           size;
    uint8_t      *canvas_buf;
    lv_timer_t   *timer;
    int           canvas_w;
    int           canvas_h;
} particle_data_t;

/* --------- 前向声明 --------- */
static void      parse_node(cJSON *node, lv_obj_t *parent);
static void      apply_common_style(cJSON *node, lv_obj_t *obj);
static void      apply_anim(cJSON *anim_node, lv_obj_t *obj);
static void      register_id(const char *id, lv_obj_t *obj);
static void      action_event_cb(lv_event_t *e);
static void      dispatch_action(const char *uri, const char *widget_id);

/* ======================================================
 * 工具函数
 * ====================================================== */

static lv_color_t parse_color(const char *hex_str) {
    if (!hex_str || hex_str[0] != '#' || strlen(hex_str) < 7)
        return lv_color_white();
    return lv_color_hex((uint32_t)strtol(hex_str + 1, NULL, 16));
}

static lv_align_t parse_align(const char *s) {
    if (!s) return LV_ALIGN_DEFAULT;
    if (!strcmp(s, "center"))       return LV_ALIGN_CENTER;
    if (!strcmp(s, "top_mid"))      return LV_ALIGN_TOP_MID;
    if (!strcmp(s, "top_left"))     return LV_ALIGN_TOP_LEFT;
    if (!strcmp(s, "top_right"))    return LV_ALIGN_TOP_RIGHT;
    if (!strcmp(s, "bottom_mid"))   return LV_ALIGN_BOTTOM_MID;
    if (!strcmp(s, "bottom_left"))  return LV_ALIGN_BOTTOM_LEFT;
    if (!strcmp(s, "bottom_right")) return LV_ALIGN_BOTTOM_RIGHT;
    if (!strcmp(s, "left_mid"))     return LV_ALIGN_LEFT_MID;
    if (!strcmp(s, "right_mid"))    return LV_ALIGN_RIGHT_MID;
    return LV_ALIGN_DEFAULT;
}

static lv_flex_flow_t parse_flex_flow(const char *s) {
    if (!s) return LV_FLEX_FLOW_COLUMN;
    if (!strcmp(s, "row"))         return LV_FLEX_FLOW_ROW;
    if (!strcmp(s, "column"))      return LV_FLEX_FLOW_COLUMN;
    if (!strcmp(s, "row_wrap"))    return LV_FLEX_FLOW_ROW_WRAP;
    if (!strcmp(s, "column_wrap")) return LV_FLEX_FLOW_COLUMN_WRAP;
    return LV_FLEX_FLOW_COLUMN;
}

static lv_flex_align_t parse_flex_align(const char *s) {
    if (!s) return LV_FLEX_ALIGN_START;
    if (!strcmp(s, "start"))         return LV_FLEX_ALIGN_START;
    if (!strcmp(s, "end"))           return LV_FLEX_ALIGN_END;
    if (!strcmp(s, "center"))        return LV_FLEX_ALIGN_CENTER;
    if (!strcmp(s, "space_evenly"))  return LV_FLEX_ALIGN_SPACE_EVENLY;
    if (!strcmp(s, "space_around"))  return LV_FLEX_ALIGN_SPACE_AROUND;
    if (!strcmp(s, "space_between")) return LV_FLEX_ALIGN_SPACE_BETWEEN;
    return LV_FLEX_ALIGN_START;
}

static lv_coord_t parse_size_value(cJSON *item) {
    if (!item) return LV_SIZE_CONTENT;
    if (cJSON_IsNumber(item)) return (lv_coord_t)item->valueint;
    if (cJSON_IsString(item)) {
        const char *s = item->valuestring;
        if (!strcmp(s, "full"))    return lv_pct(100);
        if (!strcmp(s, "content")) return LV_SIZE_CONTENT;
        int len = strlen(s);
        if (len > 1 && s[len - 1] == '%') return lv_pct(atoi(s));
        return (lv_coord_t)atoi(s);
    }
    return LV_SIZE_CONTENT;
}

/* ======================================================
 * 公共样式应用
 * ====================================================== */
static void apply_common_style(cJSON *node, lv_obj_t *obj) {
    cJSON *w = cJSON_GetObjectItem(node, "w");
    cJSON *h = cJSON_GetObjectItem(node, "h");
    if (w) lv_obj_set_width(obj,  parse_size_value(w));
    if (h) lv_obj_set_height(obj, parse_size_value(h));

    cJSON *align = cJSON_GetObjectItem(node, "align");
    if (align && cJSON_IsString(align)) {
        int xo = 0, yo = 0;
        cJSON *xi = cJSON_GetObjectItem(node, "x");
        cJSON *yi = cJSON_GetObjectItem(node, "y");
        if (xi) xo = xi->valueint;
        if (yi) yo = yi->valueint;
        lv_obj_align(obj, parse_align(align->valuestring), xo, yo);
    }

    cJSON *bg_color = cJSON_GetObjectItem(node, "bg_color");
    if (bg_color && cJSON_IsString(bg_color)) {
        lv_obj_set_style_bg_color(obj, parse_color(bg_color->valuestring), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    cJSON *bg_opa = cJSON_GetObjectItem(node, "bg_opa");
    if (bg_opa && cJSON_IsNumber(bg_opa))
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)bg_opa->valueint, 0);

    cJSON *pad = cJSON_GetObjectItem(node, "pad");
    if (pad && cJSON_IsNumber(pad))
        lv_obj_set_style_pad_all(obj, (lv_coord_t)pad->valueint, 0);

    cJSON *radius = cJSON_GetObjectItem(node, "radius");
    if (radius && cJSON_IsNumber(radius))
        lv_obj_set_style_radius(obj, (lv_coord_t)radius->valueint, 0);

    cJSON *gap = cJSON_GetObjectItem(node, "gap");
    if (gap && cJSON_IsNumber(gap)) {
        lv_obj_set_style_pad_row(obj,    (lv_coord_t)gap->valueint, 0);
        lv_obj_set_style_pad_column(obj, (lv_coord_t)gap->valueint, 0);
    }

    cJSON *bw = cJSON_GetObjectItem(node, "border_w");
    if (bw && cJSON_IsNumber(bw))
        lv_obj_set_style_border_width(obj, (lv_coord_t)bw->valueint, 0);
    cJSON *bc = cJSON_GetObjectItem(node, "border_color");
    if (bc && cJSON_IsString(bc))
        lv_obj_set_style_border_color(obj, parse_color(bc->valuestring), 0);

    cJSON *tc = cJSON_GetObjectItem(node, "text_color");
    if (tc && cJSON_IsString(tc))
        lv_obj_set_style_text_color(obj, parse_color(tc->valuestring), 0);

    cJSON *fs = cJSON_GetObjectItem(node, "font_size");
    if (fs && cJSON_IsNumber(fs)) {
        int sz = fs->valueint;
        const lv_font_t *font = &lv_font_montserrat_14;
        if (sz >= 26)      font = &lv_font_montserrat_26;
        else if (sz >= 24) font = &lv_font_montserrat_24;
        else if (sz >= 20) font = &lv_font_montserrat_20;
        else if (sz >= 16) font = &lv_font_montserrat_16;
        lv_obj_set_style_text_font(obj, font, 0);
    }

    /* 阴影 */
    cJSON *sw = cJSON_GetObjectItem(node, "shadow_w");
    if (sw && cJSON_IsNumber(sw))
        lv_obj_set_style_shadow_width(obj, sw->valueint, 0);
    cJSON *sc = cJSON_GetObjectItem(node, "shadow_color");
    if (sc && cJSON_IsString(sc))
        lv_obj_set_style_shadow_color(obj, parse_color(sc->valuestring), 0);

    /* 整体不透明度 */
    cJSON *opa = cJSON_GetObjectItem(node, "opa");
    if (opa && cJSON_IsNumber(opa))
        lv_obj_set_style_opa(obj, (lv_opa_t)opa->valueint, 0);

    cJSON *hidden = cJSON_GetObjectItem(node, "hidden");
    if (hidden && cJSON_IsTrue(hidden))
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* ======================================================
 * 事件回调与 Action URI 分发
 * ====================================================== */
static void action_event_cb(lv_event_t *e) {
    action_data_t *ad = (action_data_t *)lv_event_get_user_data(e);
    if (!ad) return;
    lv_event_code_t code   = lv_event_get_code(e);
    lv_obj_t       *target = lv_event_get_target(e);
    const char     *wid    = "unknown";
    for (int i = 0; i < s_id_count; i++) {
        if (s_id_table[i].obj == target) { wid = s_id_table[i].id; break; }
    }
    if (code == LV_EVENT_CLICKED  && ad->on_click[0])   dispatch_action(ad->on_click,   wid);
    if (code == LV_EVENT_PRESSED  && ad->on_press[0])   dispatch_action(ad->on_press,   wid);
    if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && ad->on_release[0])
        dispatch_action(ad->on_release, wid);
}

static void dispatch_action(const char *uri, const char *wid) {
    if (!uri || !uri[0]) return;
    char pl[128];
    snprintf(pl, sizeof(pl), "{\"id\":\"%s\"}", wid);
    if      (strncmp(uri, "local://",  8) == 0) sdui_bus_publish_local(uri + 8, pl);
    else if (strncmp(uri, "server://", 9) == 0) sdui_bus_publish_up(uri + 9, pl);
    else                                         sdui_bus_publish_up("ui/click", pl);
}

static void bind_actions(cJSON *node, lv_obj_t *obj) {
    cJSON *oc = cJSON_GetObjectItem(node, "on_click");
    cJSON *op = cJSON_GetObjectItem(node, "on_press");
    cJSON *or = cJSON_GetObjectItem(node, "on_release");
    if (!oc && !op && !or) return;
    action_data_t *ad = calloc(1, sizeof(action_data_t));
    if (!ad) return;
    if (oc && cJSON_IsString(oc)) strncpy(ad->on_click,   oc->valuestring, 63);
    if (op && cJSON_IsString(op)) strncpy(ad->on_press,   op->valuestring, 63);
    if (or && cJSON_IsString(or)) strncpy(ad->on_release, or->valuestring, 63);
    lv_obj_add_event_cb(obj, action_event_cb, LV_EVENT_ALL, ad);
}

/* ======================================================
 * ID 注册表
 * ====================================================== */
static void register_id(const char *id, lv_obj_t *obj) {
    if (s_id_count >= MAX_ID_ENTRIES) return;
    strncpy(s_id_table[s_id_count].id, id, 31);
    s_id_table[s_id_count].obj = obj;
    s_id_count++;
}
static void clear_id_table(void) {
    s_id_count = 0;
    memset(s_id_table, 0, sizeof(s_id_table));
}

/* ======================================================
 * 删除回调（资源释放）
 * ====================================================== */
static void free_action_data_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}
static void free_image_data_cb(lv_event_t *e) {
    image_data_t *d = lv_event_get_user_data(e);
    if (d) { heap_caps_free(d->data_buf); free(d); }
}
static void free_slider_data_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}
static void free_color_anim_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}
static void spin_delete_cb(lv_event_t *e) {
    (void)e;
    if (s_spin_count > 0) s_spin_count--;
}
static void free_particle_data_cb(lv_event_t *e) {
    particle_data_t *pd = lv_event_get_user_data(e);
    if (!pd) return;
    if (pd->timer) { lv_timer_delete(pd->timer); pd->timer = NULL; }
    if (pd->canvas_buf) heap_caps_free(pd->canvas_buf);
    free(pd);
}

/* ======================================================
 * 动画 exec_cb 函数
 * ====================================================== */
static void anim_opa_exec_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
static void anim_translate_x_exec_cb(void *var, int32_t v) {
    lv_obj_set_style_translate_x((lv_obj_t *)var, v, 0);
}
static void anim_translate_y_exec_cb(void *var, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}
static void anim_rotation_exec_cb(void *var, int32_t v) {
    lv_image_set_rotation((lv_obj_t *)var, (int16_t)v);
}
static void anim_color_exec_cb(void *var, int32_t v) {
    color_anim_data_t *cad = (color_anim_data_t *)var;
    lv_color_t c = lv_color_mix(cad->color_b, cad->color_a, (uint8_t)v);
    lv_obj_set_style_bg_color(cad->obj, c, 0);
    lv_obj_set_style_bg_opa(cad->obj, LV_OPA_COVER, 0);
}

/* ======================================================
 * 动画驱动：apply_anim
 * ====================================================== */
static void apply_anim(cJSON *an, lv_obj_t *obj) {
    if (!an || !obj) return;
    cJSON *ti = cJSON_GetObjectItem(an, "type");
    if (!ti || !cJSON_IsString(ti)) return;
    const char *atype = ti->valuestring;

    cJSON *di = cJSON_GetObjectItem(an, "duration");
    int32_t dur = (di && cJSON_IsNumber(di)) ? di->valueint : 1000;

    cJSON *ri = cJSON_GetObjectItem(an, "repeat");
    int32_t  rep_raw = (ri && cJSON_IsNumber(ri)) ? ri->valueint : -1;
    uint32_t repeat  = (rep_raw < 0) ? LV_ANIM_REPEAT_INFINITE : (uint32_t)rep_raw;

    lv_anim_t a;
    lv_anim_init(&a);

    /* ---------- blink ---------- */
    if (!strcmp(atype, "blink")) {
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, anim_opa_exec_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, repeat);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    /* ---------- breathe ---------- */
    else if (!strcmp(atype, "breathe")) {
        cJSON *mni = cJSON_GetObjectItem(an, "min_opa");
        cJSON *mxi = cJSON_GetObjectItem(an, "max_opa");
        int32_t mn = (mni && cJSON_IsNumber(mni)) ? mni->valueint : 80;
        int32_t mx = (mxi && cJSON_IsNumber(mxi)) ? mxi->valueint : 255;
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, anim_opa_exec_cb);
        lv_anim_set_values(&a, mn, mx);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, (repeat == 0) ? LV_ANIM_REPEAT_INFINITE : repeat);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    /* ---------- spin (image only) ---------- */
    else if (!strcmp(atype, "spin")) {
        if (!lv_obj_has_class(obj, &lv_image_class)) {
            ESP_LOGW(TAG, "anim:spin only for image widget, skipped");
            return;
        }
        if (s_spin_count >= MAX_SPIN_ANIM) {
            ESP_LOGW(TAG, "anim:spin limit reached (%d), degraded", MAX_SPIN_ANIM);
            return;
        }
        cJSON *dri = cJSON_GetObjectItem(an, "direction");
        bool ccw = (dri && cJSON_IsString(dri) && !strcmp(dri->valuestring, "ccw"));
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, anim_rotation_exec_cb);
        lv_anim_set_values(&a, ccw ? 3600 : 0, ccw ? 0 : 3600);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_repeat_count(&a, (repeat == 0) ? LV_ANIM_REPEAT_INFINITE : repeat);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        s_spin_count++;
        lv_obj_add_event_cb(obj, spin_delete_cb, LV_EVENT_DELETE, NULL);
    }

    /* ---------- slide_in ---------- */
    else if (!strcmp(atype, "slide_in")) {
        cJSON *fromi = cJSON_GetObjectItem(an, "from");
        const char *from = (fromi && cJSON_IsString(fromi)) ? fromi->valuestring : "left";
        bool is_x = (!strcmp(from, "left") || !strcmp(from, "right"));
        bool negative = (!strcmp(from, "left")  || !strcmp(from, "top"));
        int32_t offset = negative ? -SDUI_SCREEN_W : SDUI_SCREEN_W;
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, is_x ? anim_translate_x_exec_cb : anim_translate_y_exec_cb);
        lv_anim_set_values(&a, offset, 0);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    /* ---------- shake ---------- */
    else if (!strcmp(atype, "shake")) {
        cJSON *ampi = cJSON_GetObjectItem(an, "amplitude");
        int32_t amp = (ampi && cJSON_IsNumber(ampi)) ? ampi->valueint : 8;
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, anim_translate_x_exec_cb);
        lv_anim_set_values(&a, -amp, amp);
        lv_anim_set_duration(&a, dur / 4);
        lv_anim_set_playback_duration(&a, dur / 4);
        lv_anim_set_repeat_count(&a, 2);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    /* ---------- color_pulse ---------- */
    else if (!strcmp(atype, "color_pulse")) {
        cJSON *cai = cJSON_GetObjectItem(an, "color_a");
        cJSON *cbi = cJSON_GetObjectItem(an, "color_b");
        color_anim_data_t *cad = calloc(1, sizeof(color_anim_data_t));
        if (!cad) return;
        cad->obj     = obj;
        cad->color_a = parse_color(cai && cJSON_IsString(cai) ? cai->valuestring : "#1a1a2e");
        cad->color_b = parse_color(cbi && cJSON_IsString(cbi) ? cbi->valuestring : "#e94560");
        lv_anim_set_var(&a, cad);
        lv_anim_set_exec_cb(&a, anim_color_exec_cb);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, (repeat == 0) ? LV_ANIM_REPEAT_INFINITE : repeat);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        /* color_anim_data 由 obj 删除时回调释放 */
        lv_obj_add_event_cb(obj, free_color_anim_cb, LV_EVENT_DELETE, cad);
    }

    /* ---------- marquee (label only) ---------- */
    else if (!strcmp(atype, "marquee")) {
        if (lv_obj_has_class(obj, &lv_label_class)) {
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
        }
    }

    else {
        ESP_LOGW(TAG, "Unknown anim type: %s", atype);
    }
}

/* ======================================================
 * 粒子系统 Timer 回调
 * ====================================================== */
static void particle_timer_cb(lv_timer_t *timer) {
    particle_data_t *pd = (particle_data_t *)lv_timer_get_user_data(timer);
    if (!pd || !pd->canvas) return;

    /* 熔断机制：如果在录音期间则跳过一切消耗 SPI 总线写 PSRAM 和屏幕缓冲的回调 */
    if (audio_manager_is_recording()) {
        return;
    }

    int cw = pd->canvas_w;
    int ch = pd->canvas_h;
    int cx = cw / 2;
    int cy = ch / 2;

    /* 清除画布 */
    lv_canvas_fill_bg(pd->canvas, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(pd->canvas, &layer);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color  = pd->color;
    dsc.radius    = pd->size;
    dsc.border_width = 0;

    for (int i = 0; i < pd->count; i++) {
        particle_t *p = &pd->p[i];
        if (!p->active) {
            /* 重置粒子到中心，随机初速度 */
            p->x  = (float)(rand() % 20 - 10);
            p->y  = (float)(rand() % 20 - 10);
            p->vx = ((rand() % 200) - 100) / 80.0f;
            p->vy = ((rand() % 200) - 100) / 80.0f - 1.5f;
            p->alpha  = 255;
            p->active = true;
        }
        p->x  += p->vx;
        p->y  += p->vy;
        p->vy += 0.06f; /* 重力 */
        p->alpha = (p->alpha > 8) ? (p->alpha - 8) : 0;
        if (p->alpha == 0) { p->active = false; continue; }

        int px = cx + (int)p->x - pd->size;
        int py = cy + (int)p->y - pd->size;
        if (px >= 0 && py >= 0 && px < cw && py < ch) {
            lv_area_t area = {px, py, px + pd->size * 2, py + pd->size * 2};
            dsc.bg_opa = p->alpha;
            lv_draw_rect(&layer, &dsc, &area);
        }
    }
    lv_canvas_finish_layer(pd->canvas, &layer);
}

/* ======================================================
 * 创建 container 组件
 * ====================================================== */
static lv_obj_t *create_container(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);

    cJSON *flex = cJSON_GetObjectItem(node, "flex");
    if (flex && cJSON_IsString(flex)) {
        lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(cont, parse_flex_flow(flex->valuestring));
    }
    cJSON *just = cJSON_GetObjectItem(node, "justify");
    cJSON *ali  = cJSON_GetObjectItem(node, "align_items");
    if (just || ali) {
        lv_flex_align_t ma = just && cJSON_IsString(just) ? parse_flex_align(just->valuestring) : LV_FLEX_ALIGN_START;
        lv_flex_align_t ca = ali  && cJSON_IsString(ali)  ? parse_flex_align(ali->valuestring)  : LV_FLEX_ALIGN_START;
        lv_obj_set_flex_align(cont, ma, ca, ca);
    }
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    /* scrollable 属性 */
    cJSON *sc = cJSON_GetObjectItem(node, "scrollable");
    if (sc && cJSON_IsTrue(sc)) {
        lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ACTIVE);
    } else {
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    }
    return cont;
}

/* ======================================================
 * 创建 label 组件
 * ====================================================== */
static lv_obj_t *create_label(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    cJSON *text = cJSON_GetObjectItem(node, "text");
    lv_label_set_text(label, (text && cJSON_IsString(text)) ? text->valuestring : "");

    cJSON *lm = cJSON_GetObjectItem(node, "long_mode");
    if (lm && cJSON_IsString(lm)) {
        if      (!strcmp(lm->valuestring, "wrap"))   lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        else if (!strcmp(lm->valuestring, "scroll")) lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL);
        else if (!strcmp(lm->valuestring, "dot"))    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        else if (!strcmp(lm->valuestring, "marquee")) lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    return label;
}

/* ======================================================
 * 创建 button 组件
 * ====================================================== */
static lv_obj_t *create_button(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    cJSON *text = cJSON_GetObjectItem(node, "text");
    if (text && cJSON_IsString(text)) {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text->valuestring);
        lv_obj_center(lbl);
        cJSON *tc = cJSON_GetObjectItem(node, "text_color");
        if (tc && cJSON_IsString(tc))
            lv_obj_set_style_text_color(lbl, parse_color(tc->valuestring), 0);
        cJSON *fs = cJSON_GetObjectItem(node, "font_size");
        if (fs && cJSON_IsNumber(fs)) {
            int sz = fs->valueint;
            const lv_font_t *font = &lv_font_montserrat_14;
            if (sz >= 26)      font = &lv_font_montserrat_26;
            else if (sz >= 24) font = &lv_font_montserrat_24;
            else if (sz >= 20) font = &lv_font_montserrat_20;
            else if (sz >= 16) font = &lv_font_montserrat_16;
            lv_obj_set_style_text_font(lbl, font, 0);
        }
    }
    return btn;
}

/* ======================================================
 * 创建 image 组件（Base64 → RGB565 raw）
 * ====================================================== */
static lv_obj_t *create_image(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *img = lv_image_create(parent);

    cJSON *src_item = cJSON_GetObjectItem(node, "src");
    cJSON *iw_item  = cJSON_GetObjectItem(node, "img_w");
    cJSON *ih_item  = cJSON_GetObjectItem(node, "img_h");

    if (src_item && cJSON_IsString(src_item) && iw_item && ih_item) {
        const char *b64    = src_item->valuestring;
        size_t      b64len = strlen(b64);
        size_t      out_len = 0;

        /* 计算解码后的字节数 */
        mbedtls_base64_decode(NULL, 0, &out_len, (const unsigned char *)b64, b64len);
        if (out_len > 0) {
            uint8_t *buf = (uint8_t *)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                ESP_LOGW(TAG, "image: PSRAM alloc failed (%d bytes)", out_len);
            } else {
                size_t actual = 0;
                int ret = mbedtls_base64_decode(buf, out_len, &actual,
                                                (const unsigned char *)b64, b64len);
                if (ret == 0) {
                    image_data_t *idata = calloc(1, sizeof(image_data_t));
                    if (idata) {
                        idata->data_buf              = buf;
                        idata->dsc.data              = buf;
                        idata->dsc.data_size         = actual;
                        idata->dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
                        idata->dsc.header.w          = iw_item->valueint;
                        idata->dsc.header.h          = ih_item->valueint;
                        idata->dsc.header.stride     = iw_item->valueint * 2;
                        lv_image_set_src(img, &idata->dsc);
                        lv_obj_add_event_cb(img, free_image_data_cb, LV_EVENT_DELETE, idata);
                    } else {
                        heap_caps_free(buf);
                    }
                } else {
                    ESP_LOGW(TAG, "image: base64 decode failed (%d)", ret);
                    heap_caps_free(buf);
                }
            }
        }
    }

    /* pivot 居中（旋转原点） */
    lv_image_set_pivot(img, lv_obj_get_width(img) / 2, lv_obj_get_height(img) / 2);
    return img;
}

/* ======================================================
 * 创建 bar 组件
 * ====================================================== */
static lv_obj_t *create_bar(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 200, 20);  /* 默认尺寸，可被 common_style 覆盖 */

    cJSON *mn = cJSON_GetObjectItem(node, "min");
    cJSON *mx = cJSON_GetObjectItem(node, "max");
    int32_t min_v = (mn && cJSON_IsNumber(mn)) ? mn->valueint : 0;
    int32_t max_v = (mx && cJSON_IsNumber(mx)) ? mx->valueint : 100;
    lv_bar_set_range(bar, min_v, max_v);

    cJSON *val = cJSON_GetObjectItem(node, "value");
    if (val && cJSON_IsNumber(val))
        lv_bar_set_value(bar, val->valueint, LV_ANIM_ON);

    cJSON *bgc = cJSON_GetObjectItem(node, "bg_color");
    if (bgc && cJSON_IsString(bgc)) {
        lv_obj_set_style_bg_color(bar, parse_color(bgc->valuestring), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    cJSON *ic = cJSON_GetObjectItem(node, "indic_color");
    if (ic && cJSON_IsString(ic)) {
        lv_obj_set_style_bg_color(bar, parse_color(ic->valuestring), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    return bar;
}

/* ======================================================
 * 创建 slider 组件
 * ====================================================== */
static void slider_changed_cb(lv_event_t *e) {
    slider_data_t *sd = (slider_data_t *)lv_event_get_user_data(e);
    if (!sd || sd->on_change[0] == '\0') return;
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t   v      = lv_slider_get_value(slider);
    char      pl[128];
    snprintf(pl, sizeof(pl), "{\"id\":\"%s\",\"value\":%ld}", sd->id, (long)v);
    if      (strncmp(sd->on_change, "local://",  8) == 0) sdui_bus_publish_local(sd->on_change + 8, pl);
    else if (strncmp(sd->on_change, "server://", 9) == 0) sdui_bus_publish_up(sd->on_change + 9,   pl);
    else                                                    sdui_bus_publish_up("ui/action",          pl);
}

static lv_obj_t *create_slider(cJSON *node, lv_obj_t *parent) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_width(slider, 200);

    cJSON *mn  = cJSON_GetObjectItem(node, "min");
    cJSON *mx  = cJSON_GetObjectItem(node, "max");
    cJSON *val = cJSON_GetObjectItem(node, "value");
    lv_slider_set_range(slider,
        (mn && cJSON_IsNumber(mn)) ? mn->valueint : 0,
        (mx && cJSON_IsNumber(mx)) ? mx->valueint : 100);
    if (val && cJSON_IsNumber(val))
        lv_slider_set_value(slider, val->valueint, LV_ANIM_OFF);

    cJSON *oc = cJSON_GetObjectItem(node, "on_change");
    if (oc && cJSON_IsString(oc)) {
        slider_data_t *sd = calloc(1, sizeof(slider_data_t));
        if (sd) {
            strncpy(sd->on_change, oc->valuestring, 63);
            /* 从注册表中找当前 id */
            cJSON *id_item = cJSON_GetObjectItem(node, "id");
            if (id_item && cJSON_IsString(id_item))
                strncpy(sd->id, id_item->valuestring, 31);
            lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_RELEASED, sd);
            lv_obj_add_event_cb(slider, free_slider_data_cb, LV_EVENT_DELETE, sd);
        }
    }
    return slider;
}

/* ======================================================
 * 创建 particle 组件
 * ====================================================== */
static lv_obj_t *create_particle(cJSON *node, lv_obj_t *parent) {
    cJSON *cw_item = cJSON_GetObjectItem(node, "canvas_w");
    cJSON *ch_item = cJSON_GetObjectItem(node, "canvas_h");
    int cw = (cw_item && cJSON_IsNumber(cw_item)) ? cw_item->valueint : 200;
    int ch = (ch_item && cJSON_IsNumber(ch_item)) ? ch_item->valueint : 200;
    /* 限制最大尺寸：200×200×2B = 80KB PSRAM */
    if (cw > 200) cw = 200;
    if (ch > 200) ch = 200;

    size_t buf_sz = cw * ch * 2; /* RGB565 */
    uint8_t *buf  = (uint8_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "particle: PSRAM alloc failed (%d bytes)", buf_sz);
        return lv_obj_create(parent); /* 返回空占位 */
    }
    memset(buf, 0, buf_sz);

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, cw, ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, cw, ch);

    particle_data_t *pd = calloc(1, sizeof(particle_data_t));
    if (!pd) { heap_caps_free(buf); return canvas; }

    pd->canvas     = canvas;
    pd->canvas_buf = buf;
    pd->canvas_w   = cw;
    pd->canvas_h   = ch;

    cJSON *cnt_i  = cJSON_GetObjectItem(node, "count");
    cJSON *col_i  = cJSON_GetObjectItem(node, "color");
    cJSON *sz_i   = cJSON_GetObjectItem(node, "particle_size");
    cJSON *dur_i  = cJSON_GetObjectItem(node, "duration");
    pd->count  = (cnt_i && cJSON_IsNumber(cnt_i)) ? LV_MIN(cnt_i->valueint, MAX_PARTICLES) : 20;
    pd->color  = parse_color((col_i && cJSON_IsString(col_i)) ? col_i->valuestring : "#ffffff");
    pd->size   = (sz_i && cJSON_IsNumber(sz_i)) ? sz_i->valueint : 3;
    int period = (dur_i && cJSON_IsNumber(dur_i)) ? dur_i->valueint : 33; /* ~30fps */

    pd->timer  = lv_timer_create(particle_timer_cb, period, pd);

    lv_obj_add_event_cb(canvas, free_particle_data_cb, LV_EVENT_DELETE, pd);
    return canvas;
}

/* ======================================================
 * 递归解析节点
 * ====================================================== */
static void parse_node(cJSON *node, lv_obj_t *parent) {
    if (!node || !parent) return;
    cJSON *type = cJSON_GetObjectItem(node, "type");
    if (!type || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Node missing 'type', skipped");
        return;
    }
    const char *ts  = type->valuestring;
    lv_obj_t   *obj = NULL;

    if      (!strcmp(ts, "container")) obj = create_container(node, parent);
    else if (!strcmp(ts, "label"))     obj = create_label(node, parent);
    else if (!strcmp(ts, "button"))    obj = create_button(node, parent);
    else if (!strcmp(ts, "image"))     obj = create_image(node, parent);
    else if (!strcmp(ts, "bar"))       obj = create_bar(node, parent);
    else if (!strcmp(ts, "slider"))    obj = create_slider(node, parent);
    else if (!strcmp(ts, "particle"))  obj = create_particle(node, parent);
    else {
        ESP_LOGW(TAG, "Unknown widget type: %s", ts);
        return;
    }
    if (!obj) return;

    /* 注册 ID */
    cJSON *id = cJSON_GetObjectItem(node, "id");
    if (id && cJSON_IsString(id)) register_id(id->valuestring, obj);

    /* 应用通用样式 */
    apply_common_style(node, obj);

    /* 绑定 Action URI */
    bind_actions(node, obj);

    /* 注册删除回调 */
    lv_obj_add_event_cb(obj, free_action_data_cb, LV_EVENT_DELETE, NULL);

    /* 应用动画 */
    cJSON *anim = cJSON_GetObjectItem(node, "anim");
    if (anim && cJSON_IsObject(anim)) apply_anim(anim, obj);

    /* 递归子节点 */
    cJSON *children = cJSON_GetObjectItem(node, "children");
    if (children && cJSON_IsArray(children)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, children) {
            parse_node(child, obj);
        }
    }
}

/* ======================================================
 * 过渡动画辅助：根视图 Fade-In
 * ====================================================== */
static void root_fade_in(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root_view);
    lv_anim_set_exec_cb(&a, anim_opa_exec_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* ======================================================
 * 公共 API
 * ====================================================== */
lv_obj_t *sdui_parser_init(void) {
    lv_obj_t *scr = lv_scr_act();

    /* 禁用活动屏幕本身的 scrollable 和 scrollbar，
     * 防止其在内容稍有溢出时于右下角渲染灰色滚动条方块 */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_root_view   = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root_view);
    lv_obj_set_size(s_root_view,
                    SDUI_SCREEN_W - 2 * SDUI_SAFE_PADDING,
                    SDUI_SCREEN_H - 2 * SDUI_SAFE_PADDING);
    lv_obj_center(s_root_view);
    lv_obj_set_style_bg_opa(s_root_view, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_root_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(s_root_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_root_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root_view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    clear_id_table();
    ESP_LOGI(TAG, "Parser init. Root: %dx%d, safe_pad=%d",
             SDUI_SCREEN_W - 2*SDUI_SAFE_PADDING,
             SDUI_SCREEN_H - 2*SDUI_SAFE_PADDING,
             SDUI_SAFE_PADDING);
    return s_root_view;
}

lv_obj_t *sdui_parser_get_root(void) { return s_root_view; }

void sdui_parser_render(const char *json_str) {
    if (!json_str || !s_root_view) return;
    ESP_LOGI(TAG, "Render layout (%d bytes)", strlen(json_str));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) { ESP_LOGE(TAG, "JSON parse failed"); return; }

    /* --- 过渡动画：先瞬间隐藏，渲染完毕后 Fade-In --- */
    lv_obj_set_style_opa(s_root_view, LV_OPA_TRANSP, 0);

    /* 清除旧 UI 树 */
    lv_obj_clean(s_root_view);
    clear_id_table();
    s_spin_count = 0;

    /* 重置根视图 Flex */
    lv_obj_set_layout(s_root_view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_root_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root_view, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(s_root_view, LV_OPA_TRANSP, 0);

    /* 解析 JSON 树 */
    if (cJSON_IsArray(root)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) parse_node(item, s_root_view);
    } else if (cJSON_IsObject(root)) {
        cJSON *children = cJSON_GetObjectItem(root, "children");
        if (children && cJSON_IsArray(children)) {
            apply_common_style(root, s_root_view);
            cJSON *flex = cJSON_GetObjectItem(root, "flex");
            if (flex && cJSON_IsString(flex)) {
                lv_obj_set_layout(s_root_view, LV_LAYOUT_FLEX);
                lv_obj_set_flex_flow(s_root_view, parse_flex_flow(flex->valuestring));
            }
            cJSON *just = cJSON_GetObjectItem(root, "justify");
            cJSON *ali  = cJSON_GetObjectItem(root, "align_items");
            if (just || ali) {
                lv_flex_align_t ma = just && cJSON_IsString(just) ? parse_flex_align(just->valuestring) : LV_FLEX_ALIGN_CENTER;
                lv_flex_align_t ca = ali  && cJSON_IsString(ali)  ? parse_flex_align(ali->valuestring)  : LV_FLEX_ALIGN_CENTER;
                lv_obj_set_flex_align(s_root_view, ma, ca, ca);
            }
            cJSON *child = NULL;
            cJSON_ArrayForEach(child, children) parse_node(child, s_root_view);
        } else {
            parse_node(root, s_root_view);
        }
    }

    cJSON_Delete(root);

    /* 渲染完毕后 Fade-In */
    root_fade_in();
    ESP_LOGI(TAG, "Render done. IDs: %d", s_id_count);
}

lv_obj_t *sdui_parser_find_by_id(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < s_id_count; i++) {
        if (!strcmp(s_id_table[i].id, id)) return s_id_table[i].obj;
    }
    return NULL;
}

void sdui_parser_update(const char *json_str) {
    if (!json_str) return;
    cJSON *root = cJSON_Parse(json_str);
    if (!root) { ESP_LOGW(TAG, "update: JSON parse failed"); return; }

    cJSON *id_item = cJSON_GetObjectItem(root, "id");
    if (!id_item || !cJSON_IsString(id_item)) {
        ESP_LOGW(TAG, "update: missing 'id'");
        cJSON_Delete(root);
        return;
    }
    lv_obj_t *target = sdui_parser_find_by_id(id_item->valuestring);
    if (!target) {
        ESP_LOGW(TAG, "update: widget '%s' not found", id_item->valuestring);
        cJSON_Delete(root);
        return;
    }

    /* text */
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && cJSON_IsString(text)) {
        lv_obj_t *lobj = target;
        if (lv_obj_get_child_count(target) > 0)
            lobj = lv_obj_get_child(target, 0);
        lv_label_set_text(lobj, text->valuestring);
    }

    /* hidden */
    cJSON *hidden = cJSON_GetObjectItem(root, "hidden");
    if (hidden) {
        if (cJSON_IsTrue(hidden)) lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
        else                      lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
    }

    /* bg_color */
    cJSON *bgc = cJSON_GetObjectItem(root, "bg_color");
    if (bgc && cJSON_IsString(bgc)) {
        lv_obj_set_style_bg_color(target, parse_color(bgc->valuestring), 0);
        lv_obj_set_style_bg_opa(target, LV_OPA_COVER, 0);
    }

    /* value（bar / slider 专用） */
    cJSON *val = cJSON_GetObjectItem(root, "value");
    if (val && cJSON_IsNumber(val)) {
        if (lv_obj_has_class(target, &lv_bar_class)) {
            lv_bar_set_value(target, val->valueint, LV_ANIM_ON);
        } else if (lv_obj_has_class(target, &lv_slider_class)) {
            lv_slider_set_value(target, val->valueint, LV_ANIM_ON);
        }
    }

    /* indic_color (bar) */
    cJSON *ic = cJSON_GetObjectItem(root, "indic_color");
    if (ic && cJSON_IsString(ic) && lv_obj_has_class(target, &lv_bar_class)) {
        lv_obj_set_style_bg_color(target, parse_color(ic->valuestring), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(target, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    /* opa */
    cJSON *opa = cJSON_GetObjectItem(root, "opa");
    if (opa && cJSON_IsNumber(opa))
        lv_obj_set_style_opa(target, (lv_opa_t)opa->valueint, 0);

    /* 触发动画 */
    cJSON *anim = cJSON_GetObjectItem(root, "anim");
    if (anim && cJSON_IsObject(anim)) apply_anim(anim, target);

    ESP_LOGI(TAG, "Updated '%s'", id_item->valuestring);
    cJSON_Delete(root);
}
