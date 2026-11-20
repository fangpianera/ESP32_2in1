idf_component_register(
    SRCS
        "main.c"
        "screen_pm.c"
    INCLUDE_DIRS "."
    # 选择界面底图：466x466 RGB565（434,312 B），编进固件随镜像一起烧
    EMBED_FILES
        "assets/sel_bg.bin"
    PRIV_REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75c
        esp_partition
        app_update
        nvs_flash
        esp_timer
)
