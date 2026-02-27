#include "audio_manager.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "mbedtls/base64.h"
#include "sdui_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "AUDIO_MANAGER";

static esp_codec_dev_handle_t spk_handle = NULL;
static esp_codec_dev_handle_t mic_handle = NULL;
static bool is_recording = false;

#define PCM_CHUNK_SIZE 1024

// ========== I2S 引脚与宏定义（复用 BSP 头文件中的常量） ==========
#define AUDIO_I2S_GPIO_CFG       \
    {                            \
        .mclk = BSP_I2S_MCLK,   \
        .bclk = BSP_I2S_SCLK,   \
        .ws = BSP_I2S_LCLK,     \
        .dout = BSP_I2S_DOUT,   \
        .din = BSP_I2S_DSIN,    \
        .invert_flags = {        \
            .mclk_inv = false,   \
            .bclk_inv = false,   \
            .ws_inv = false,     \
        },                       \
    }

#define AUDIO_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                   \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                            \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),   \
        .gpio_cfg = AUDIO_I2S_GPIO_CFG,                                                                 \
    }

// 下行音频流回调：由 sdui_bus 路由到此处
static void audio_play_callback(const char *base64_data)
{
    ESP_LOGI(TAG, "Audio data received, len: %d", strlen(base64_data));
    
    if (!spk_handle || !base64_data)
        return;

    size_t data_len = strlen(base64_data);
    // pcm_buf 属于高频实时操作缓冲，强制分配到内部 SRAM 防止 PSRAM 带宽被占用导致的 I2S 缺载失真
    unsigned char *pcm_buf = (unsigned char *)heap_caps_malloc(data_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t pcm_len = 0;

    if (pcm_buf)
    {
        int ret = mbedtls_base64_decode(pcm_buf, data_len, &pcm_len, (const unsigned char *)base64_data, data_len);
        if (ret == 0 && pcm_len > 0)
        {
            esp_codec_dev_write(spk_handle, pcm_buf, pcm_len);
        }
        else
        {
            ESP_LOGE(TAG, "Base64 decode failed: %d", ret);
        }
        heap_caps_free(pcm_buf);
    }
}

// 后台上行录音任务
static void audio_record_task(void *arg)
{
    ESP_LOGI(TAG, "audio_record_task started on core %d", xPortGetCoreID());
    // 强制把直接与硬件打交道的 pcm_buf 分配到内部 SRAM，以应对极高实时性要求
    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(PCM_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Base64 和 JSON 组装缓冲区，不直接对硬件，分配到默认空间（PSRAM）
    unsigned char *base64_buf = (unsigned char *)heap_caps_malloc(1500, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    char *json_buf = (char *)heap_caps_malloc(2048, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);

    if (!pcm_buf || !base64_buf || !json_buf)
    {
        ESP_LOGE(TAG, "Failed to allocate internal memory! System halted.");
        if (pcm_buf)
            heap_caps_free(pcm_buf);
        if (base64_buf)
            heap_caps_free(base64_buf);
        if (json_buf)
            heap_caps_free(json_buf);
        vTaskDelete(NULL);
    }

    size_t base64_len = 0;

    while (1)
    {
        if (is_recording && mic_handle)
        {
            esp_err_t ret = esp_codec_dev_read(mic_handle, pcm_buf, PCM_CHUNK_SIZE);

            if (ret == ESP_OK)
            {
                // 打印前 4 个字节，看是否有波动（如果配置为双声道，可分辨左右声道）
                ESP_LOGI(TAG, "Debug PCM - L: %02x %02x | R: %02x %02x", pcm_buf[0], pcm_buf[1], pcm_buf[2], pcm_buf[3]);

                mbedtls_base64_encode(base64_buf, 1500, &base64_len, pcm_buf, PCM_CHUNK_SIZE);
                base64_buf[base64_len] = '\0';

                // 组装总线 payload
                snprintf(json_buf, 2048, "{\"state\": \"stream\", \"data\": \"%s\"}", base64_buf);
                sdui_bus_publish_up("audio/record", json_buf);
            }
            else
            {
                ESP_LOGE(TAG, "I2S read error: %d", ret);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void audio_record_start(void)
{
    if (!is_recording)
    {
        ESP_LOGI(TAG, "Recording started...");
        sdui_bus_publish_up("audio/record", "{\"state\": \"start\"}");
        is_recording = true;
    }
}

void audio_record_stop(void)
{
    if (is_recording)
    {
        is_recording = false;
        sdui_bus_publish_up("audio/record", "{\"state\": \"stop\"}");
        ESP_LOGI(TAG, "Recording stopped.");
    }
}

bool audio_manager_is_recording(void)
{
    return is_recording;
}

void audio_app_start(void)
{
    ESP_LOGI(TAG, "Initializing Audio subsystem (using official BSP)...");

    // 直接调用官方 BSP 提供的扬声器与麦克风初始化
    // 这将正确初始化 I2S 并且分别使用 ES8311(DAC) 和 ES7210(ADC)
    spk_handle = bsp_audio_codec_speaker_init();
    mic_handle = bsp_audio_codec_microphone_init();

    if (spk_handle)
    {
        esp_codec_dev_set_out_vol(spk_handle, 70);
        esp_codec_dev_sample_info_t fs = {.sample_rate = 22050, .channel = 1, .bits_per_sample = 16};
        esp_codec_dev_open(spk_handle, &fs);
        ESP_LOGI(TAG, "Speaker ready.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create speaker device via BSP!");
    }

    if (mic_handle)
    {
        esp_codec_dev_set_in_gain(mic_handle, 24.0);
        esp_codec_dev_sample_info_t fs = {.sample_rate = 22050, .channel = 2, .bits_per_sample = 16};
        esp_codec_dev_open(mic_handle, &fs);
        ESP_LOGI(TAG, "Microphone ready (Stereo Reading Mode).");

        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            audio_record_task,
            "audio_record_task",
            4096,
            NULL,
            2,
            NULL,
            1,
            MALLOC_CAP_SPIRAM);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio_record_task (SPIRAM stack), err=%d", ret);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to create microphone device via BSP!");
    }

    // 订阅云端下发的音频指令
    sdui_bus_subscribe("audio/play", audio_play_callback);
}