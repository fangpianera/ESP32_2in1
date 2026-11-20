/**
 * board_pins.h — ESP32-S3-Touch-AMOLED-1.75C 板级引脚基线
 *
 * 证据来源（三处官方源码交叉一致，均为 [官方源码] 级证据，非凭记忆）：
 *  1) examples/arduino/libraries/board_config/pin_config.h
 *       LCD_SDIO0 4 / SDIO1 5 / SDIO2 6 / SDIO3 7 / SCLK 38 / RESET 1 / CS 12
 *       IIC_SDA 15 / IIC_SCL 14 / TP_INT 11 / TP_RST 2 / LCD 466x466 / PA 46
 *  2) waveshare_boards/boards/esp32_s3_touch_amoled_1_75c/board_peripherals.yaml
 *       spi_display: spi_port=SPI2_HOST, mosi(SDIO0)=4, miso(SDIO1)=5,
 *                    quadwp(SDIO2)=6, quadhd(SDIO3)=7, sclk=38, max_transfer_sz=9320(=466*2*10)
 *       i2c_master: port 0, sda 15, scl 14
 *  3) 同上 board_devices.yaml
 *       display_lcd: chip co5300, cs_gpio_num=12, spi_mode=0, lcd_cmd_bits=32,
 *                    flags.quad_mode=true, reset_gpio_num=1, x_max=y_max=466
 *       lcd_touch: chip cst9217, i2c_addr 0xB4(>>1=0x5A), rst=2, int=11,
 *                  flags: swap_xy=false, mirror_x=true, mirror_y=true
 *       axp2101 : i2c_addr 0x34
 *
 * 注意：QMI8658 的器件地址在官方头文件中命名反直觉
 *      (QMI8658Constants.h: QMI8658_L_SLAVE_ADDRESS=0x6B, QMI8658_H_SLAVE_ADDRESS=0x6A)，
 *      因此本工程不在编译期硬编码，改为上电时以 WHO_AM_I(0x00)==0x05 探测（见 imu_pdr.c）。
 */
#pragma once

/* ---- QSPI 段（CO5300 AMOLED 466x466）---- */
#define EVA_LCD_SDIO0        4    /* SPI2 MOSI / QSPI IO0 */
#define EVA_LCD_SDIO1        5    /* SPI2 MISO / QSPI IO1 */
#define EVA_LCD_SDIO2        6    /* SPI2 WP   / QSPI IO2 */
#define EVA_LCD_SDIO3        7    /* SPI2 HD   / QSPI IO3 */
#define EVA_LCD_SCLK         38
#define EVA_LCD_CS           12
#define EVA_LCD_RST          1
#define EVA_LCD_H_RES        466
#define EVA_LCD_V_RES        466

/* ---- I2C0 段（AXP2101 / QMI8658 / CST9217 / ES8311）---- */
#define EVA_I2C_PORT         0
#define EVA_I2C_SDA          15
#define EVA_I2C_SCL          14

/* ---- 触摸 ---- */
#define EVA_TP_INT           11
#define EVA_TP_RST           2

/* ---- 其它 ---- */
#define EVA_PA_GPIO          46
#define EVA_BOOT_GPIO        0    /* BOOT 键，用于“重新校准 / 重新配网”触发 */

/* 器件 I2C 地址（7bit） */
#define EVA_ADDR_AXP2101     0x34
#define EVA_ADDR_CST9217     0x5A
#define EVA_ADDR_QMI8658_A   0x6A
#define EVA_ADDR_QMI8658_B   0x6B
