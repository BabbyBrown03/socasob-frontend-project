#include "lcdTask.h"

static esp_lcd_panel_io_handle_t lcd_io = NULL;
esp_lcd_panel_handle_t lcd_panel = NULL;

static const char* TAG_DISPLAY = "DISPLAY";

esp_err_t lcd_spi_bus_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_GPIO_SCLK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t)};
    return spi_bus_initialize(LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO);
}

esp_err_t lcd_init(void)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGD(TAG_DISPLAY, "Initialize SPI bus for ST7735");
    ESP_GOTO_ON_ERROR(lcd_spi_bus_init(), err, TAG_DISPLAY, "SPI bus init failed");

    ESP_LOGD(TAG_DISPLAY, "Install LCD panel IO (SPI)");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10};
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_NUM, &io_config, &lcd_io),
                      err, TAG_DISPLAY, "Panel IO init failed");

    ESP_LOGD(TAG_DISPLAY, "Install ST7735 panel driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7735(lcd_io, &panel_config, &lcd_panel),
                      err, TAG_DISPLAY, "ST7735 driver init failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG_DISPLAY, "Panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG_DISPLAY, "Panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_set_gap(lcd_panel, 2, 3), err, TAG_DISPLAY, "Set gap failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_swap_xy(lcd_panel, true), err, TAG_DISPLAY, "Swap XY failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, false, true), err, TAG_DISPLAY, "Mirror failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), err, TAG_DISPLAY, "Turn on display failed");
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG_DISPLAY, "ST7735 LCD init success");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = LCD_H_RES * 20,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .flags = {
            .buff_dma = true,
            .sw_rotate = false,
        }};

    lvgl_port_add_disp(&disp_cfg);

    return ret;

err:
    if (lcd_panel != NULL) {
        esp_lcd_panel_del(lcd_panel);
        lcd_panel = NULL;
    }
    if (lcd_io != NULL) {
        esp_lcd_panel_io_del(lcd_io);
        lcd_io = NULL;
    }
    spi_bus_free(LCD_SPI_NUM);
    ESP_LOGE(TAG_DISPLAY, "ST7735 LCD init failed (err: %s)", esp_err_to_name(ret));
    return ret;
}
