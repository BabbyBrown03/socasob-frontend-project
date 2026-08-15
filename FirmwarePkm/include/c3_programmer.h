#ifndef C3_PROGRAMMER_H
#define C3_PROGRAMMER_H

#include "esp_err.h"

/**
 * @brief Inisialisasi UART & pin kontrol (boot/reset) untuk memprogram
 *        ESP32-C3 dari ESP32-S (host) ini. Panggil sekali di awal, sebelum
 *        c3_programmer_flash_from_urls().
 */
esp_err_t c3_programmer_init(void);

/**
 * @brief Download 3 file firmware (bootloader, partition table, app) dari
 *        server HTTP, lalu flash langsung ke ESP32-C3 lewat UART tanpa
 *        perlu menyimpan file ke storage lokal dulu (streaming).
 *
 *        Setelah selesai, ESP32-C3 otomatis di-reset untuk boot normal
 *        (bukan mode bootloader lagi).
 *
 * @param bootloader_url  URL file bootloader.bin C3 (biasanya di-flash ke 0x0000)
 * @param partition_url   URL file partition-table.bin (biasanya di 0x8000)
 * @param app_url         URL file aplikasi .bin C3 (biasanya di 0x10000)
 */
esp_err_t c3_programmer_flash_from_urls(const char *bootloader_url,
                                         const char *partition_url,
                                         const char *app_url);

#endif // C3_PROGRAMMER_H