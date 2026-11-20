# 说明：qmi8658 / lvgl 由 main/idf_component.yml 声明，组件管理器自动挂到 main 依赖上；
# 但 BSP 已 vendor 到 firmware/components/（为打开 psram_dma_direct），
# 走的是本地组件路径，必须在 PRIV_REQUIRES 里显式写目录名，否则 bsp/esp-bsp.h 找不到。
idf_component_register(
    SRCS
        "app_main.c"
        "net_link.c"
        "ble_link.c"
        "proto.c"
        "route_store.c"
        "imu_pdr.c"
        "ekf.c"
        "nav_core.c"
        "ui_eva.c"
        "res_mgr.c"
        # 板载 AXP2101 电源管理：电量 + 充电状态
        "power_axp.c"
        # AMOLED 息屏节能（防烧屏）：15s 降亮 / 20s 熄屏，触摸或 BLE 帧唤醒
        "screen_pm.c"
        # GB2312 全字库（7614 字 + ASCII，Noto Sans SC / OFL）
        # 原来只收 ~500 个高频汉字，目的地名一出现生僻字就渲染成方块，故整库内嵌。
        #   18px：目标地 / 剩余里程 / 状态等次级信息
        #   24px：中心区"当前前进路线"（用户要求比目的地更大、更粗、更醒目）
        "fonts/cjk18.c"
        "fonts/cjk24.c"
        # EVA 机甲字体（Chakra Petch，OFL）：48/20/15 三档，纯 ASCII + 度数符号
        "fonts/eva_tech_48.c"
        "fonts/eva_tech_20.c"
        "fonts/eva_tech_15.c"
    INCLUDE_DIRS "."
    # 资源镜像：图标与字体以二进制形式编入固件，首次启动自举写入 LittleFS 资源分区
    # （避免为了放少量文件去格式化 20MB 的 storage 分区）。
    EMBED_FILES
        "assets/nerv64.bin"
        "assets/nerv_bg.bin"
        "assets/num48.bin"
    PRIV_REQUIRES
        waveshare__esp32_s3_touch_amoled_1_75c
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_littlefs
        lwip
        json
        nvs_flash
        bt
        # app_main.c 用 esp_ota_ops.h 做 A/B OTA 分区切换，必须显式依赖
        app_update
)
