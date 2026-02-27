idf_component_register(SRCS "audio_manager.c"
                    INCLUDE_DIRS "include"
                    # 必须使用真实长名称，并引入 mbedtls 库
                    REQUIRES websocket_manager waveshare__esp32_s3_touch_amoled_1_75c mbedtls espressif__esp_codec_dev driver)
