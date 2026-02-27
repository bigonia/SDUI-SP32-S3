idf_component_register(SRCS "websocket_manager.c"
                    INCLUDE_DIRS "include"
                    # 追加 audio_manager 依赖，使编译器暴露对应头文件
                    REQUIRES esp_websocket_client json audio_manager esp_timer sdui_bus)