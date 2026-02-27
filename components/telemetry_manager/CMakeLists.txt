idf_component_register(
    SRCS "telemetry_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sdui_bus
        esp_wifi
        esp_netif
        esp_timer
        esp_system
        freertos
        driver
        json
)
