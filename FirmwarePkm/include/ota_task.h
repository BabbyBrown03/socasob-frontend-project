#ifndef OTA_TASK_H
#define OTA_TASK_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Task FreeRTOS yang mengecek & memasang firmware baru secara
 *        berkala dari OTA_URL. Cocok dipakai dengan xTaskCreate().
 *        Panggil HANYA setelah WiFi konek.
 */
void vTaskOtaCheck(void *pvParameters);

/**
 * @brief Jalankan OTA sekali langsung dari URL tertentu (blocking).
 *        Berguna untuk trigger manual, mis. dari perintah UDP/HTTP.
 */
esp_err_t start_ota_update(const char *url);

/**
 * @brief Konfirmasi firmware yang sedang berjalan valid, supaya bootloader
 *        tidak melakukan rollback otomatis ke firmware sebelumnya saat reset.
 *        WAJIB dipanggil sekali di awal app_main().
 */
void ota_confirm_running_app(void);

#endif // OTA_TASK_H