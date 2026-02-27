#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <string.h>
#include <esp_http_server.h>

// 硬编码的网络配置（后备）
#define ESP_WIFI_SSID      "ZWDSJ"
#define ESP_WIFI_PASS      "zwdsj888"
#define ESP_WS_URL         "ws://172.16.11.64:8080"
#define ESP_MAXIMUM_RETRY  5

// 配网配置
#define PROV_WIFI_SSID     "SDUI-Setup"
#define PROV_WIFI_PASS     "12345678"

static const char *TAG = "WIFI_MANAGER";
static int s_retry_num = 0;
static char s_ip_str[16] = "0.0.0.0";   // 缓存 IP 地址

// 缓存配置信息
static char s_ssid[32] = {0};
static char s_password[64] = {0};
static char s_ws_url[128] = {0};

/* ---- NVS 存取辅助函数 ---- */

static void load_config_from_nvs(void)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS storage not found or uninitialized. Using default values.");
        strncpy(s_ssid, ESP_WIFI_SSID, sizeof(s_ssid));
        strncpy(s_password, ESP_WIFI_PASS, sizeof(s_password));
        strncpy(s_ws_url, ESP_WS_URL, sizeof(s_ws_url));
        return;
    }

    size_t length = sizeof(s_ssid);
    err = nvs_get_str(my_handle, "ssid", s_ssid, &length);
    if (err != ESP_OK) {
        strncpy(s_ssid, ESP_WIFI_SSID, sizeof(s_ssid));
    }

    length = sizeof(s_password);
    err = nvs_get_str(my_handle, "password", s_password, &length);
    if (err != ESP_OK) {
        strncpy(s_password, ESP_WIFI_PASS, sizeof(s_password));
    }

    length = sizeof(s_ws_url);
    err = nvs_get_str(my_handle, "ws_url", s_ws_url, &length);
    if (err != ESP_OK) {
        strncpy(s_ws_url, ESP_WS_URL, sizeof(s_ws_url));
    }

    nvs_close(my_handle);
}

static esp_err_t save_config_to_nvs(const char* ssid, const char* pass, const char* ws)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(my_handle, "ssid", ssid);
    nvs_set_str(my_handle, "password", pass);
    nvs_set_str(my_handle, "ws_url", ws);

    err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}


/* ---- HTTP Web Server 路由 ---- */

// 简单的 URL 解码
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    char html[2048];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html>\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "  <title>SDUI Device Setup</title>\n"
        "  <style>\n"
        "    body{font-family:sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}\n"
        "    .card{background:#fff;padding:2rem;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);width:90%%;max-width:400px;}\n"
        "    h2{text-align:center;color:#333;margin-top:0;}\n"
        "    .form-group{margin-bottom:1.2rem;}\n"
        "    label{display:block;margin-bottom:0.5rem;color:#666;font-size:0.9rem;}\n"
        "    input{width:100%%;padding:0.8rem;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:1rem;}\n"
        "    input:focus{outline:none;border-color:#3498db;}\n"
        "    .btn{width:100%%;padding:1rem;background:#3498db;color:#fff;border:none;border-radius:6px;font-size:1.1rem;cursor:pointer;margin-top:1rem;}\n"
        "    .btn:hover{background:#2980b9;}\n"
        "    .tips{font-size:0.8rem;color:#888;margin-top:1.5rem;text-align:center;}\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"card\">\n"
        "    <h2>Wi-Fi 配网</h2>\n"
        "    <form action=\"/save\" method=\"POST\">\n"
        "      <div class=\"form-group\">\n"
        "        <label>Wi-Fi SSID</label>\n"
        "        <input type=\"text\" name=\"ssid\" value=\"%s\" required>\n"
        "      </div>\n"
        "      <div class=\"form-group\">\n"
        "        <label>Wi-Fi Password</label>\n"
        "        <input type=\"text\" name=\"password\" value=\"%s\">\n"
        "      </div>\n"
        "      <div class=\"form-group\">\n"
        "        <label>Server URL (WebSocket)</label>\n"
        "        <input type=\"text\" name=\"ws_url\" value=\"%s\" required>\n"
        "      </div>\n"
        "      <button type=\"submit\" class=\"btn\">保存并重启</button>\n"
        "    </form>\n"
        "    <div class=\"tips\">保存后系统将自动重启并连接配置的网络</div>\n"
        "  </div>\n"
        "</body>\n"
        "</html>\n", s_ssid, s_password, s_ws_url);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[512] = {0};
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[remaining] = '\0';

    char new_ssid[64] = {0};
    char new_password[64] = {0};
    char new_ws_url[128] = {0};

    char* pairs[3];
    int count = 0;
    char* token = strtok(buf, "&");
    while (token != NULL && count < 3) {
        pairs[count++] = token;
        token = strtok(NULL, "&");
    }

    for (int i = 0; i < count; i++) {
        char* key = strtok(pairs[i], "=");
        char* val = strtok(NULL, "=");
        if (key && val) {
            if (strcmp(key, "ssid") == 0) {
                url_decode(new_ssid, val);
            } else if (strcmp(key, "password") == 0) {
                url_decode(new_password, val);
            } else if (strcmp(key, "ws_url") == 0) {
                url_decode(new_ws_url, val);
            }
        }
    }

    ESP_LOGI(TAG, "Received Prov Data: SSID='%s', PASS='%s', WS='%s'", new_ssid, new_password, new_ws_url);
    
    // 固化凭证
    if (strlen(new_ssid) > 0) {
        save_config_to_nvs(new_ssid, new_password, new_ws_url);
        ESP_LOGI(TAG, "Config saved to NVS. Restarting in 2s...");
        
        const char resp[] = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
                            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                            "<title>Saved</title></head><body style=\"text-align:center;font-family:sans-serif;padding:2rem;\">"
                            "<h2>配置已保存</h2><p>设备即将重启并连接指定的网络，请关闭此页面。</p></body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_send_500(req);
    }
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    // 对特定的请求（如果不仅是 / 和 /save），执行 302 跳转
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t uri_get = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

static httpd_uri_t uri_post = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_post_handler,
    .user_ctx  = NULL
};

// 捕获所有未匹配的 GET (使用通配符匹配)
static httpd_uri_t uri_captive = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = captive_redirect_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard; // 开启通配符匹配功能

    ESP_LOGI(TAG, "Starting provision web server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_post);
        httpd_register_uri_handler(server, &uri_captive); // 兜底的 Captive 跳转
        return server;
    }
    return NULL;
}


/* ---- 事件回调函数：处理 WiFi 状态跃迁 ---- */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi station started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retry to connect to the AP (%d/%d)", s_retry_num, ESP_MAXIMUM_RETRY);
        } else {
            ESP_LOGE(TAG, "Failed to connect to WiFi after max retries.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP Address: %s", s_ip_str);
        s_retry_num = 0;
    }
}

/* ---- 核心 API ---- */

bool wifi_manager_is_provisioned(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        return false; // 无 storage 分区
    }

    size_t length = 0;
    err = nvs_get_str(my_handle, "ssid", NULL, &length);
    nvs_close(my_handle);

    // 如果能读到 ssid 且有长度，视为已配网
    return (err == ESP_OK && length > 1);
}

/* ---- Captive Portal DNS Server ---- */
static void captive_dns_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS Server started on port 53");

    char rx_buffer[128];
    char tx_buffer[128];

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        // 解析基本 DNS 查询（Transaction ID, Flags, Questions...）
        // 我们只需将请求复制回响应，修改 Flags 为回应 (0x8180)，并附加一个 A 记录 (Type=1)
        if (len > 12) {
            memcpy(tx_buffer, rx_buffer, len);
            
            // Flags: 0x8180 (Standard query response, No error)
            tx_buffer[2] = 0x81;
            tx_buffer[3] = 0x80;
            // Answer count = 1
            tx_buffer[6] = 0x00;
            tx_buffer[7] = 0x01;

            int tx_len = len;

            // 附加 Answer
            // Name: 以指针格式重用查询中的名字 (0xC00C)
            tx_buffer[tx_len++] = 0xC0;
            tx_buffer[tx_len++] = 0x0C;
            // Type: A (1)
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x01;
            // Class: IN (1)
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x01;
            // TTL: 60s
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x3C;
            // Data length: 4
            tx_buffer[tx_len++] = 0x00;
            tx_buffer[tx_len++] = 0x04;
            // IP: 192.168.4.1
            tx_buffer[tx_len++] = 192;
            tx_buffer[tx_len++] = 168;
            tx_buffer[tx_len++] = 4;
            tx_buffer[tx_len++] = 1;

            sendto(sock, tx_buffer, tx_len, 0, (struct sockaddr *)&source_addr, socklen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

void wifi_manager_start_provision(void)
{
    // 初始化配置回退值
    load_config_from_nvs();

    // 初始化网络
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = PROV_WIFI_SSID,
            .ssid_len = strlen(PROV_WIFI_SSID),
            .password = PROV_WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(PROV_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP Provisioning started. SSID:%s PASS:%s", PROV_WIFI_SSID, PROV_WIFI_PASS);

    // 启动 HTTP Web Server
    start_webserver();

    // 启动 Captive Portal DNS，拦截手机的探测请求
    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 5, NULL);
}

void wifi_init_sta(void)
{
    load_config_from_nvs();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // 使用 NVS 中的配置
    strncpy((char*)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished. SSID:%s", s_ssid);
}

void wifi_manager_get_ws_url(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    load_config_from_nvs();
    strncpy(buf, s_ws_url, len - 1);
    buf[len - 1] = '\0';
}

/* ---- 查询接口实现 ---- */

int wifi_get_rssi(void)
{
    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return (int)ap_info.rssi;
    }
    return 0;
}

void wifi_get_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strncpy(buf, s_ip_str, len - 1);
    buf[len - 1] = '\0';
}
