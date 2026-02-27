#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "wifi_manager.h"
#include "websocket_manager.h"
#include "imu_manager.h"
#include "audio_manager.h"
#include "sdui_bus.h"
#include "sdui_parser.h"
#include "telemetry_manager.h"

static const char *TAG = "SDUI_APP";

// 息屏控制变量
#define SCREEN_SLEEP_TIMEOUT_MS 30000 
static bool is_screen_sleeping = false;

/* ---- SDUI 总线回调：处理 ui/layout 主题（全量布局渲染） ---- */
static void on_ui_layout(const char *payload)
{
    if (!payload) return;
    bsp_display_lock(-1);
    lv_disp_trig_activity(NULL);
    sdui_parser_render(payload);
    bsp_display_unlock();
}

/* ---- SDUI 总线回调：处理 ui/update 主题（增量属性更新） ---- */
static void on_ui_update(const char *payload)
{
    if (!payload) return;
    bsp_display_lock(-1);
    lv_disp_trig_activity(NULL);
    sdui_parser_update(payload);
    bsp_display_unlock();
}

/* ---- SDUI 总线回调：处理 audio/cmd/record_start（本地事件路由） ---- */
static void on_audio_record_start(const char *payload)
{
    ESP_LOGI(TAG, "Bus event -> audio record start");
    audio_record_start();
}

/* ---- SDUI 总线回调：处理 audio/cmd/record_stop（本地事件路由） ---- */
static void on_audio_record_stop(const char *payload)
{
    ESP_LOGI(TAG, "Bus event -> audio record stop");
    audio_record_stop();
}

/* ---- 息屏检测轮询任务 ---- */
static void screen_sleep_timer_cb(lv_timer_t * timer)
{
    uint32_t inactive_time = lv_disp_get_inactive_time(NULL);

    if (inactive_time > SCREEN_SLEEP_TIMEOUT_MS) {
        if (!is_screen_sleeping) {
            ESP_LOGI(TAG, "Screen inactive. Sleeping...");
            bsp_display_brightness_set(0); 
            is_screen_sleeping = true;
        }
    } else {
        if (is_screen_sleeping) {
            ESP_LOGI(TAG, "Screen activity detected. Waking up...");
            bsp_display_brightness_set(100);
            is_screen_sleeping = false;
        }
    }
}

/* ---- 默认首屏 Loading 界面 (WebSocket 连接前展示） ---- */
static void build_loading_screen(void)
{
    lv_obj_t *root = sdui_parser_get_root();
    if (!root) return;

    lv_obj_clean(root);
    lv_obj_t *spinner = lv_spinner_create(root);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_center(spinner);

    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, "Connecting...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 50);
}

/* ---- 配网界面提示 (SoftAP 模式下展示） ---- */
static void build_provisioning_screen(void)
{
    lv_obj_t *root = sdui_parser_get_root();
    if (!root) return;
    
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);

    lv_obj_t *container = lv_obj_create(root);
    lv_obj_set_size(container, 320, 320);
    lv_obj_center(container);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 160, 0); 
    lv_obj_set_style_pad_all(container, 20, 0);

    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 10, 0); // 缩小行间距

    lv_obj_t *title = lv_label_create(container);
    lv_label_set_text(title, "Wi-Fi Setup");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x3498db), 0);

    lv_obj_t *step1 = lv_label_create(container);
    lv_label_set_text(step1, "1. Connect Wi-Fi:");
    lv_obj_set_style_text_font(step1, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(step1, lv_color_hex(0xcccccc), 0);

    lv_obj_t *ssid = lv_label_create(container);
    lv_label_set_text(ssid, "SDUI-Setup");
    lv_obj_set_style_text_font(ssid, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ssid, lv_color_hex(0xffffff), 0);

    lv_obj_t *pwd = lv_label_create(container);
    lv_label_set_text(pwd, "PWD: 12345678");
    lv_obj_set_style_text_font(pwd, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pwd, lv_color_hex(0xffaa00), 0);

    lv_obj_t *step2 = lv_label_create(container);
    lv_label_set_text(step2, "2. Browser to:");
    lv_obj_set_style_text_font(step2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(step2, lv_color_hex(0xcccccc), 0);

    lv_obj_t *ip = lv_label_create(container);
    lv_label_set_text(ip, "192.168.4.1");
    lv_obj_set_style_text_font(ip, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ip, lv_color_hex(0x2ecc71), 0);
}

void app_main(void)
{
    // 1. 硬件初始化：显示优先（SPI DMA 需要内部 SRAM，必须最先分配）
    bsp_display_start();

    // 2. 初始化 SDUI 解析引擎
    bsp_display_lock(-1);
    sdui_parser_init();
    
    // 挂载息屏定时器 (每 500ms 检查一次)
    lv_timer_create(screen_sleep_timer_cb, 500, NULL);
    bsp_display_unlock();

    // 检查是否已配网
    bool is_provisioned = wifi_manager_is_provisioned();

    if (!is_provisioned) {
        // ==== 配网态 (Provisioning State) ====
        ESP_LOGW(TAG, "Device not provisioned. Entering Provisioning Mode.");
        
        bsp_display_lock(-1);
        build_provisioning_screen();
        bsp_display_unlock();

        // 启动配网 Web 服务器
        wifi_manager_start_provision();

        // 阻塞主循环，等待用户在网页操作后软重启 (esp_restart)
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // ==== 托管态 (Cloud State) ====
    ESP_LOGI(TAG, "Device provisioned. Starting Cloud State.");

    bsp_display_lock(-1);
    build_loading_screen();
    bsp_display_unlock();

    // 3. 初始化 SDUI 消息总线 (必须在各组件订阅前优先初始化)
    sdui_bus_init();

    // 4. 初始化音频子系统 (I2S DMA 必须在 Wi-Fi 导致 SRAM 碎片化之前尽早分配)
    audio_app_start();

    //    -- 下行 UI 主题 --
    sdui_bus_subscribe("ui/layout", on_ui_layout);   // 全量布局渲染
    sdui_bus_subscribe("ui/update", on_ui_update);   // 增量属性更新

    //    -- 本地硬件事件主题 (由 Action URI local:// 触发) --
    sdui_bus_subscribe("audio/cmd/record_start", on_audio_record_start);                
    sdui_bus_subscribe("audio/cmd/record_stop",  on_audio_record_stop);

    // 5. 启动网络系统（会导致 SRAM 严重碎片化）
    wifi_init_sta();
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 获取 NVS 中保存的 WebSocket URL
    char ws_url[128] = {0};
    wifi_manager_get_ws_url(ws_url, sizeof(ws_url));
    if (strlen(ws_url) == 0) {
        strncpy(ws_url, "ws://172.16.11.64:8080", sizeof(ws_url)); // Fallback
    }
    ESP_LOGI(TAG, "Connecting to WebSocket: %s", ws_url);

    // 6. 启动外围子系统
    websocket_app_start(ws_url, sdui_bus_route_down); 
    imu_app_start();

    // 7. 启动遥测上报模块（每 30 秒上报一次设备状态）
    // 必须在 websocket_app_start() 之后调用，确保上行链路就绪
    telemetry_app_start(30);
}