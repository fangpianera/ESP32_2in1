/**
 * display_co5300_qspi_reference.c —— 参考实现（**不参与本工程编译**）
 *
 * 用途：不想依赖 waveshare BSP、想自己掌控 CO5300 初始化时用这份。
 * 主工程走官方 BSP（bsp_display_start()），因为 BSP 已经处理了
 *   CO5300 上电时序、QSPI 打包、双缓冲、触摸、背光与 AXP2101 电源域，
 * 自研驱动属于"能跑但没必要"的路线（doc §12 坑 1）。
 *
 * 所有数值来源：
 *   - 引脚/io 配置：waveshare_boards/boards/esp32_s3_touch_amoled_1_75c/board_devices.yaml
 *   - 初始化命令与参数：GFX_Library_for_Arduino/src/display/Arduino_CO5300.h L121-149
 *   - 命令码：同文件 L15-102（CO5300_C_SLPOUT=0x11 等）
 * 未标注来源的值一律标 [待确认]，禁止照抄进量产固件。
 */
#if defined(EVA_BUILD_REFERENCE_PANEL)

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_types.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_pins.h"

#define LCD_HOST        SPI2_HOST
#define LCD_PCLK_HZ     (80 * 1000 * 1000)   /* [待确认] CO5300 QSPI 上限，先用 80MHz 验证 */

/* CO5300 命令码（Arduino_CO5300.h） */
#define CO5300_C_SLPOUT   0x11
#define CO5300_C_DISPON   0x29
#define CO5300_W_CASET    0x2A
#define CO5300_W_PASET    0x2B
#define CO5300_W_RAMWR    0x2C
#define CO5300_W_PIXFMT   0x3A
#define CO5300_W_WCTRLD1  0x53
#define CO5300_W_WCE      0x58
#define CO5300_W_SPIMODECTL 0xC4
#define CO5300_W_BRIGHTHBM 0x63
#define CO5300_W_BRIGHTNOR 0x51

static esp_lcd_panel_io_handle_t s_io;

static void cmd8(uint8_t cmd, const uint8_t *param, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, n ? param : NULL, n);
}

/* 厂商初始化序列（逐条对应 Arduino_CO5300.h 的 co5300_init_operations） */
static void co5300_init_sequence(void)
{
    cmd8(CO5300_C_SLPOUT, NULL, 0);              /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));              /* CO5300_SLPOUT_DELAY = 120ms */

    uint8_t v;
    v = 0x00; cmd8(0xFE, &v, 1);                 /* 命令集选择 */
    v = 0x80; cmd8(CO5300_W_SPIMODECTL, &v, 1);  /* SPI 模式控制（QSPI 使能位） */
    v = 0x55; cmd8(CO5300_W_PIXFMT, &v, 1);      /* 16bit/pixel（RGB565） */
    v = 0x20; cmd8(CO5300_W_WCTRLD1, &v, 1);     /* 亮度控制开 */
    v = 0xFF; cmd8(CO5300_W_BRIGHTHBM, &v, 1);   /* HBM 亮度上限 */

    cmd8(CO5300_C_DISPON, NULL, 0);              /* Display ON */
    v = 0xD0; cmd8(CO5300_W_BRIGHTNOR, &v, 1);   /* 正常模式亮度 0xD0 */
    v = 0x00; cmd8(CO5300_W_WCE, &v, 1);          /* 阳光可读性增强：关 */
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t eva_panel_init(void)
{
    /* 1) QSPI 总线：SDIO0..3 分别对应 mosi/miso/wp/hd（board_peripherals.yaml） */
    spi_bus_config_t bus = {
        .sclk_io_num = EVA_LCD_SCLK,
        .mosi_io_num = EVA_LCD_SDIO0,
        .miso_io_num = EVA_LCD_SDIO1,
        .quadwp_io_num = EVA_LCD_SDIO2,
        .quadhd_io_num = EVA_LCD_SDIO3,
        .max_transfer_sz = EVA_LCD_H_RES * 2 * 10,   /* 与 BSP 的 9320 一致 */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), "panel", "bus");

    /* 2) panel IO：lcd_cmd_bits=32 + quad_mode=true（QSPI 命令走 4 线） */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = EVA_LCD_CS,
        .dc_gpio_num = -1,                /* QSPI 无 DC 线，命令由 32bit 包头区分 */
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,              /* [待确认] QSPI 下参数位宽，需与 esp_lcd_co5300 v2 对齐 */
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_cfg, &s_io), "panel", "io");
    return ESP_OK;
}

/* 复位：RST=GPIO1，低电平有效；含 200ms 复位保持（CO5300_RST_DELAY） */
void eva_panel_reset(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << EVA_LCD_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(EVA_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EVA_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
}

/* 刷一块区域（RGB565），供 LVGL flush_cb 调用 */
void eva_panel_draw_bitmap(int x1, int y1, int x2, int y2, const void *px)
{
    uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    uint8_t paset[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    esp_lcd_panel_io_tx_param(s_io, CO5300_W_CASET, caset, 4);
    esp_lcd_panel_io_tx_param(s_io, CO5300_W_PASET, paset, 4);
    esp_lcd_panel_io_tx_color(s_io, CO5300_W_RAMWR, px,
                              (size_t)(x2 - x1) * (size_t)(y2 - y1) * 2);
}

#endif /* EVA_BUILD_REFERENCE_PANEL */
