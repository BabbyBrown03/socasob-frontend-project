#ifndef INC_LCD_TASK_H_
#define INC_LCD_TASK_H_

#include "pins.h"

// Logging
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"

// Display
#include "esp_lvgl_port.h"
#include "hal/lcd_types.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define LCD_SPI_NUM (SPI2_HOST)
#define LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define LCD_CMD_BITS (8)
#define LCD_PARAM_BITS (8)
#define LCD_BITS_PER_PIXEL (16)
#define LCD_BL_ON_LEVEL (1)

#define LCD_H_RES 160
#define LCD_V_RES 128

esp_err_t lcd_spi_bus_init(void);
esp_err_t lcd_init(void);

#endif