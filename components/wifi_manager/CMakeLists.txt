# 注册组件：
# SRCS: 指定源文件
# INCLUDE_DIRS: 指定对外暴露的头文件目录
# REQUIRES: 声明依赖的核心组件（WiFi协议栈、事件循环、NVS存储）
idf_component_register(SRCS "wifi_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_wifi esp_event nvs_flash lwip)