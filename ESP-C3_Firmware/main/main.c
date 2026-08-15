#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"

#include "esp_lvgl_port.h"
#include "hal/lcd_types.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"

#include "pins.h"
#include "lcdTask.h"

#define EXAMPLE_BUFF_SIZE               1024
#define SAMPLE_RATE 44100
#define M_PI 3.14159265358979323846f
#define UART_BUF_SIZE 256

static i2s_chan_handle_t tx_chan;

extern const uint8_t pcm_start[] asm("_binary_o_pcm_start");
extern const uint8_t pcm_end[]   asm("_binary_o_pcm_end");

typedef enum {
    SOUND_NONE = 0,
    SOUND_FATIGUE,
    SOUND_DRY
} sound_type_t;

typedef enum {
    STATE_NORMAL = 0,
    STATE_TIRED,
    STATE_DRY
} face_state_t;

static QueueHandle_t sound_queue = NULL;
static face_state_t current_face_state = STATE_NORMAL;

static lv_obj_t *left_eye = NULL;
static lv_obj_t *right_eye = NULL;
static lv_obj_t *left_eyebrow = NULL;
static lv_obj_t *right_eyebrow = NULL;
static lv_obj_t *mouth = NULL;

static lv_timer_t *blink_timer = NULL;
static bool eyes_closed = false;

static void trigger_sound(sound_type_t sound)
{
    if (sound_queue != NULL) {
        xQueueSend(sound_queue, &sound, 0);
    }
}

static void i2s_example_write_task(void *args)
{
    uint16_t *buffer = calloc(EXAMPLE_BUFF_SIZE, sizeof(uint16_t));
    if (buffer == NULL) {
        printf("Failed to allocate buffer\n");
        vTaskDelete(NULL);
    }

    size_t w_bytes = 0;
    sound_type_t current_sound = SOUND_NONE;
    
    // Tone generation state variables
    uint32_t sample_index = 0;
    uint32_t note_duration_samples = 0;
    uint32_t current_note_idx = 0;
    float current_freq = 0.0f;
    
    // Tone frequencies and durations in samples
    // Fatigue Chime: E5 (659Hz) -> C5 (523Hz)
    float fatigue_freqs[] = {659.25f, 523.25f};
    uint32_t fatigue_durs[] = {SAMPLE_RATE * 2 / 10, SAMPLE_RATE * 3 / 10}; // 200ms, 300ms
    
    // Dry Eyes Chime: G5 (784Hz) -> E5 (659Hz) -> G5 (784Hz)
    float dry_freqs[] = {783.99f, 659.25f, 783.99f};
    uint32_t dry_durs[] = {SAMPLE_RATE * 15 / 100, SAMPLE_RATE * 15 / 100, SAMPLE_RATE * 25 / 100}; // 150ms, 150ms, 250ms

    while (1) {
        sound_type_t new_sound;
        if (xQueueReceive(sound_queue, &new_sound, 0) == pdTRUE) {
            current_sound = new_sound;
            sample_index = 0;
            current_note_idx = 0;
            if (current_sound == SOUND_FATIGUE) {
                current_freq = fatigue_freqs[0];
                note_duration_samples = fatigue_durs[0];
            } else if (current_sound == SOUND_DRY) {
                current_freq = dry_freqs[0];
                note_duration_samples = dry_durs[0];
            } else {
                current_freq = 0.0f;
                note_duration_samples = 0;
            }
        }

        // Fill buffer
        for (int i = 0; i < EXAMPLE_BUFF_SIZE; i++) {
            int16_t sample = 0;
            
            if (current_sound != SOUND_NONE) {
                if (current_freq > 0.0f) {
                    float val = sinf(2.0f * M_PI * current_freq * (float)sample_index / SAMPLE_RATE);
                    
                    // Prevent audio pops/clicks with linear envelopes
                    float envelope = 1.0f;
                    uint32_t remaining = note_duration_samples - sample_index;
                    if (remaining < 882) { // 20ms fade out
                        envelope = (float)remaining / 882.0f;
                    }
                    if (sample_index < 441) { // 10ms fade in
                        envelope = (float)sample_index / 441.0f;
                    }
                    
                    sample = (int16_t)(val * envelope * 12000.0f);
                }
                
                sample_index++;
                if (sample_index >= note_duration_samples) {
                    sample_index = 0;
                    current_note_idx++;
                    
                    if (current_sound == SOUND_FATIGUE) {
                        if (current_note_idx < 2) {
                            current_freq = fatigue_freqs[current_note_idx];
                            note_duration_samples = fatigue_durs[current_note_idx];
                        } else {
                            current_sound = SOUND_NONE;
                            current_freq = 0.0f;
                        }
                    } else if (current_sound == SOUND_DRY) {
                        if (current_note_idx < 3) {
                            current_freq = dry_freqs[current_note_idx];
                            note_duration_samples = dry_durs[current_note_idx];
                        } else {
                            current_sound = SOUND_NONE;
                            current_freq = 0.0f;
                        }
                    }
                }
            } else {
                sample = 0; // silence
            }
            
            buffer[i] = (uint16_t)sample;
        }

        esp_err_t err = i2s_channel_write(
            tx_chan,
            buffer,
            EXAMPLE_BUFF_SIZE * sizeof(uint16_t),
            &w_bytes,
            portMAX_DELAY);

        if (err != ESP_OK) {
            printf("i2s write failed\n");
        }
    }
}

static void i2s_example_init_std_simplex(void) {
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

    i2s_std_config_t tx_std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),

            .gpio_cfg = {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = I2S_GPIO_BCLK,
                    .ws   = I2S_GPIO_LRC,
                    .dout = I2S_GPIO_DIN,
                    .din  = I2S_GPIO_UNUSED,
                    .invert_flags = {
                            .mclk_inv = false,
                            .bclk_inv = false,
                            .ws_inv   = false,
                    },
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
}

static void set_face_state(face_state_t state)
{
    current_face_state = state;
    eyes_closed = false; // Reset blink state on transition
    
    if (state == STATE_NORMAL) {
        lv_color_t color = lv_color_make(0, 200, 255); // Neon Cyan
        
        // Eyes
        lv_obj_set_size(left_eye, 24, 30);
        lv_obj_set_style_bg_color(left_eye, color, 0);
        lv_obj_set_style_radius(left_eye, 12, 0);
        lv_obj_align(left_eye, LV_ALIGN_CENTER, -35, -10);
        
        lv_obj_set_size(right_eye, 24, 30);
        lv_obj_set_style_bg_color(right_eye, color, 0);
        lv_obj_set_style_radius(right_eye, 12, 0);
        lv_obj_align(right_eye, LV_ALIGN_CENTER, 35, -10);
        
        // Eyebrows (Horizontal)
        lv_obj_set_style_bg_color(left_eyebrow, color, 0);
        lv_obj_set_style_transform_rotation(left_eyebrow, 0, 0);
        lv_obj_set_size(left_eyebrow, 28, 6);
        lv_obj_align(left_eyebrow, LV_ALIGN_CENTER, -35, -32);
        
        lv_obj_set_style_bg_color(right_eyebrow, color, 0);
        lv_obj_set_style_transform_rotation(right_eyebrow, 0, 0);
        lv_obj_set_size(right_eyebrow, 28, 6);
        lv_obj_align(right_eyebrow, LV_ALIGN_CENTER, 35, -32);

        // Mouth (Neutral line)
        lv_obj_set_size(mouth, 40, 8);
        lv_obj_set_style_bg_color(mouth, color, 0);
        lv_obj_set_style_radius(mouth, 4, 0);
        lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 25);
    } 
    else if (state == STATE_TIRED) {
        // Tired state -> Angry robot face
        lv_color_t color = lv_color_make(255, 60, 0); // Orange/Red
        
        // Eyes (Narrowed)
        lv_obj_set_size(left_eye, 24, 16);
        lv_obj_set_style_bg_color(left_eye, color, 0);
        lv_obj_set_style_radius(left_eye, 4, 0);
        lv_obj_align(left_eye, LV_ALIGN_CENTER, -35, -5);
        
        lv_obj_set_size(right_eye, 24, 16);
        lv_obj_set_style_bg_color(right_eye, color, 0);
        lv_obj_set_style_radius(right_eye, 4, 0);
        lv_obj_align(right_eye, LV_ALIGN_CENTER, 35, -5);
        
        // Eyebrows slanted down towards center (\ /)
        lv_obj_set_style_bg_color(left_eyebrow, color, 0);
        lv_obj_set_size(left_eyebrow, 30, 8);
        lv_obj_align(left_eyebrow, LV_ALIGN_CENTER, -32, -26);
        lv_obj_set_style_transform_rotation(left_eyebrow, 250, 0); // slant down ~25 deg
        
        lv_obj_set_style_bg_color(right_eyebrow, color, 0);
        lv_obj_set_size(right_eyebrow, 30, 8);
        lv_obj_align(right_eyebrow, LV_ALIGN_CENTER, 32, -26);
        lv_obj_set_style_transform_rotation(right_eyebrow, -250, 0); // slant down ~-25 deg

        // Mouth (Frown)
        lv_obj_set_size(mouth, 36, 6);
        lv_obj_set_style_bg_color(mouth, color, 0);
        lv_obj_set_style_radius(mouth, 3, 0);
        lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 25);
    } 
    else if (state == STATE_DRY) {
        // Dry eyes state -> Shocked robot face
        lv_color_t color = lv_color_make(255, 200, 0); // Shocked Yellow
        
        // Eyes (Wide open circles)
        lv_obj_set_size(left_eye, 32, 32);
        lv_obj_set_style_bg_color(left_eye, color, 0);
        lv_obj_set_style_radius(left_eye, 16, 0);
        lv_obj_align(left_eye, LV_ALIGN_CENTER, -35, -10);
        
        lv_obj_set_size(right_eye, 32, 32);
        lv_obj_set_style_bg_color(right_eye, color, 0);
        lv_obj_set_style_radius(right_eye, 16, 0);
        lv_obj_align(right_eye, LV_ALIGN_CENTER, 35, -10);
        
        // Eyebrows (Raised high)
        lv_obj_set_style_bg_color(left_eyebrow, color, 0);
        lv_obj_set_style_transform_rotation(left_eyebrow, 0, 0);
        lv_obj_set_size(left_eyebrow, 28, 6);
        lv_obj_align(left_eyebrow, LV_ALIGN_CENTER, -35, -38);
        
        lv_obj_set_style_bg_color(right_eyebrow, color, 0);
        lv_obj_set_style_transform_rotation(right_eyebrow, 0, 0);
        lv_obj_set_size(right_eyebrow, 28, 6);
        lv_obj_align(right_eyebrow, LV_ALIGN_CENTER, 35, -38);

        // Mouth (Oval surprise)
        lv_obj_set_size(mouth, 20, 24);
        lv_obj_set_style_bg_color(mouth, color, 0);
        lv_obj_set_style_radius(mouth, 10, 0);
        lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 25);
    }
}

static void blink_timer_cb(lv_timer_t *timer)
{
    if (current_face_state != STATE_NORMAL) {
        return; // Only blink in normal state
    }

    if (!eyes_closed) {
        // Close eyes to slit
        lv_obj_set_size(left_eye, 24, 2);
        lv_obj_set_size(right_eye, 24, 2);
        eyes_closed = true;
        lv_timer_set_period(timer, 150); // Blink duration: 150ms
    } else {
        // Open eyes back up
        lv_obj_set_size(left_eye, 24, 30);
        lv_obj_set_size(right_eye, 24, 30);
        eyes_closed = false;
        
        // Next blink in 2000ms to 5000ms
        uint32_t next_blink = 2000 + (esp_random() % 3000);
        lv_timer_set_period(timer, next_blink);
    }
}

static void create_robot_face_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Left Eye
    left_eye = lv_obj_create(screen);
    lv_obj_set_size(left_eye, 24, 30);
    lv_obj_set_style_bg_color(left_eye, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_border_width(left_eye, 0, 0);
    lv_obj_set_style_radius(left_eye, 12, 0);
    lv_obj_align(left_eye, LV_ALIGN_CENTER, -35, -10);

    // Right Eye
    right_eye = lv_obj_create(screen);
    lv_obj_set_size(right_eye, 24, 30);
    lv_obj_set_style_bg_color(right_eye, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_border_width(right_eye, 0, 0);
    lv_obj_set_style_radius(right_eye, 12, 0);
    lv_obj_align(right_eye, LV_ALIGN_CENTER, 35, -10);

    // Left Eyebrow
    left_eyebrow = lv_obj_create(screen);
    lv_obj_set_size(left_eyebrow, 28, 6);
    lv_obj_set_style_bg_color(left_eyebrow, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_border_width(left_eyebrow, 0, 0);
    lv_obj_set_style_radius(left_eyebrow, 3, 0);
    lv_obj_align(left_eyebrow, LV_ALIGN_CENTER, -35, -32);

    // Right Eyebrow
    right_eyebrow = lv_obj_create(screen);
    lv_obj_set_size(right_eyebrow, 28, 6);
    lv_obj_set_style_bg_color(right_eyebrow, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_border_width(right_eyebrow, 0, 0);
    lv_obj_set_style_radius(right_eyebrow, 3, 0);
    lv_obj_align(right_eyebrow, LV_ALIGN_CENTER, 35, -32);

    // Mouth
    mouth = lv_obj_create(screen);
    lv_obj_set_size(mouth, 40, 8);
    lv_obj_set_style_bg_color(mouth, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_set_style_radius(mouth, 4, 0);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 25);

    // Apply default normal state settings
    set_face_state(STATE_NORMAL);

    // Register eye blinking timer callback
    blink_timer = lv_timer_create(blink_timer_cb, 3000, NULL);
}

static void uart_rx_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Default system console uses UART0 on ESP32-C3
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uint8_t data[UART_BUF_SIZE];
    
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char cmd = (char)data[i];
                if (cmd == 'N') {
                    ESP_LOGI("UART", "Robot face set to: NORMAL");
                    if (lvgl_port_lock(0)) {
                        set_face_state(STATE_NORMAL);
                        lvgl_port_unlock();
                    }
                } else if (cmd == 'F') {
                    ESP_LOGI("UART", "Robot face set to: EYE FATIGUE");
                    if (lvgl_port_lock(0)) {
                        set_face_state(STATE_TIRED);
                        lvgl_port_unlock();
                    }
                    trigger_sound(SOUND_FATIGUE);
                } else if (cmd == 'D') {
                    ESP_LOGI("UART", "Robot face set to: DRY EYES");
                    if (lvgl_port_lock(0)) {
                        set_face_state(STATE_DRY);
                        lvgl_port_unlock();
                    }
                    trigger_sound(SOUND_DRY);
                }
            }
        }
    }
}

void app_main(void) {
    lcd_init();
    
    // Create queue for I2S sounds
    sound_queue = xQueueCreate(10, sizeof(sound_type_t));
    if (sound_queue == NULL) {
        printf("Failed to create sound queue\n");
    }

    i2s_example_init_std_simplex();
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // Initialize robot face UI under LVGL Port Lock
    if (lvgl_port_lock(0)) {
        create_robot_face_ui();
        lvgl_port_unlock();
    }

    // Start Audio Player task
    xTaskCreate(i2s_example_write_task, "i2s_example_write_task", 4096, NULL, 5, NULL);
    
    // Start UART Command Receiver task
    xTaskCreate(uart_rx_task, "uart_rx_task", 3072, NULL, 4, NULL);
}