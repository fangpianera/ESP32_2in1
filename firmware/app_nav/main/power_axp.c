/**
 * power_axp.c — AXP2101 电量/充电状态读取（ESP32-S3-Touch-AMOLED-1.75C 板载电源管理芯片）
 *
 * 证据来源：
 *  - 微雪文档 ESP32-S3-Touch-AMOLED-1.75 / 1.75C「硬件说明」：板载 AXP2101 高集成度电源管理芯片，
 *    集成充电与电池管理；板载 3.7V MX1.25 锂电池充放电接口 [官方文档]
 *  - 本工程 board_pins.h（引官方 board_peripherals.yaml / board_devices.yaml）：axp2101 i2c_addr 0x34 [官方源码]
 *  - 寄存器地址与位域：XPowersLib `src/REG/AXP2101Constants.h` + `XPowersAXP2101.hpp` [第三方开源实现，已核对]
 *      IC_TYPE(0x03) == 0x4A                    → 芯片在线
 *      STATUS1(0x00) bit3                       → 是否接着电池 (isBatteryConnect)
 *      STATUS2(0x01) >> 5 == 0x01               → 正在充电 (isCharging)
 *      BAT_PERCENT_DATA(0xA4)                   → 电量百分比 0..100（PMU 内部燃料计，无需开 ADC）
 * 注意：0xA4 是 PMU 自己算的 SOC，不需要打开 ADC 通道，也不受我们关掉哪些 LDO 影响。
 */
#include "power_axp.h"
#include "board_pins.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "axp";

static i2c_master_dev_handle_t s_dev;
static bool  s_ok;                 /* 芯片探测成功 */
static int   s_pct = -1;           /* -1 = 无电池/未知 */
static bool  s_charging;           /* 正在充电 */

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

esp_err_t power_axp_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "i2c bus not ready");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = EVA_ADDR_AXP2101,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed", EVA_ADDR_AXP2101);
        return ESP_FAIL;
    }

    uint8_t id = 0;
    if (reg_read(0x03, &id) != ESP_OK || id != 0x4A) {
        ESP_LOGW(TAG, "AXP2101 探测失败 (id=0x%02X, 期望 0x4A)", id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    s_ok = true;
    ESP_LOGI(TAG, "AXP2101 在线 (id=0x%02X)", id);

    power_axp_poll();
    ESP_LOGI(TAG, "初始电量 %d%% (charging=%d)", s_pct, (int)s_charging);
    return ESP_OK;
}

void power_axp_poll(void)
{
    if (!s_ok || !s_dev) return;

    uint8_t st1 = 0, st2 = 0, soc = 0;
    if (reg_read(0x00, &st1) != ESP_OK) return;
    if (reg_read(0x01, &st2) != ESP_OK) return;

    bool battery_connected = (st1 >> 3) & 0x01;
    s_charging = (((st2 >> 5) & 0x07) == 0x01);

    if (!battery_connected) {
        s_pct = -1;                 /* 只插 USB、没接电池 */
        return;
    }
    if (reg_read(0xA4, &soc) != ESP_OK) return;
    if (soc > 100) {                /* 0xFF 等非法值 */
        s_pct = -1;
        return;
    }
    s_pct = (int)soc;
}

bool power_axp_ready(void) { return s_ok; }
int  power_axp_percent(void) { return s_pct; }
bool power_axp_charging(void) { return s_charging; }
