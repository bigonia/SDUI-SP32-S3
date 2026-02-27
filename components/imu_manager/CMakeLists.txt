idf_component_register(SRCS "imu_manager.c"
                    INCLUDE_DIRS "include"
                    # 使用真实长名称声明依赖，绝不能写简称 "bsp"
                    REQUIRES sdui_bus waveshare__esp32_s3_touch_amoled_1_75c waveshare__qmi8658) 

# 强制链接数学运算库
target_link_libraries(${COMPONENT_LIB} PUBLIC m)