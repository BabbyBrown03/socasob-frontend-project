#include "c3_programmer.h"
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/uart.h"

#include "esp_loader.h"
#include "esp32_port.h" // disediakan oleh komponen esp-serial-flasher (port UART ESP32)

static const char *TAG = "C3_PROGRAMMER";

// ----------------------------------------------------------------------
// Pin & UART yang dipakai HOST (ESP32-S ini) untuk bicara ke bootloader C3.
// Sesuaikan kalau wiring fisik kamu beda dari tabel di dokumentasi.
// ----------------------------------------------------------------------
#define C3_UART_PORT      UART_NUM_1
#define C3_UART_TX_PIN    GPIO_NUM_4   // -> RX0 milik C3
#define C3_UART_RX_PIN    GPIO_NUM_5   // <- TX0 milik C3
#define C3_BOOT_PIN       GPIO_NUM_26  // -> IO0 milik C3 (tarik LOW = masuk mode download)
#define C3_RESET_PIN      GPIO_NUM_25  // -> EN/RESET milik C3

#define C3_UART_BAUD_RATE 115200

// Offset flash standar untuk target ESP32-C3.
// PENTING: ini beda dari ESP32 klasik (bootloader ESP32 klasik ada di 0x1000,
// C3 di 0x0000). Kalau kamu custom partition table dengan offset app
// berbeda dari default, sesuaikan APP_OFFSET dengan punya kamu
// (cek build/partition_table/partition-table.bin project C3 kamu).
#define C3_BOOTLOADER_OFFSET  0x0000
#define C3_PARTITION_OFFSET   0x8000
#define C3_APP_OFFSET         0x10000

#define DOWNLOAD_CHUNK_SIZE   1024

static esp_loader_t s_loader;
static esp32_port_t s_port;

esp_err_t c3_programmer_init(void)
{
    s_port = (esp32_port_t){
        .port.ops     = &esp32_uart_ops,
        .baud_rate    = C3_UART_BAUD_RATE,
        .uart_port    = C3_UART_PORT,
        .uart_rx_pin  = C3_UART_RX_PIN,
        .uart_tx_pin  = C3_UART_TX_PIN,
        .reset_pin    = C3_RESET_PIN,
        .boot_pin     = C3_BOOT_PIN,
    };

    esp_loader_error_t err = esp_loader_init_serial(&s_loader, &s_port.port);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "esp_loader_init_serial gagal: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "C3 programmer siap (UART%d, TX=%d RX=%d BOOT=%d RST=%d)",
             C3_UART_PORT, C3_UART_TX_PIN, C3_UART_RX_PIN, C3_BOOT_PIN, C3_RESET_PIN);

    return ESP_OK;
}

// Download satu file lewat HTTP dan tulis langsung ke flash target C3
// secara streaming (chunk demi chunk), tanpa perlu simpan seluruh file
// di RAM/storage ESP32-S dulu.
static esp_err_t flash_one_binary_from_url(const char *url, uint32_t target_offset)
{
    ESP_LOGI(TAG, "Download & flash %s -> offset 0x%x", url, (unsigned int)target_offset);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Gagal buka koneksi HTTP: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0) {
        ESP_LOGE(TAG, "Content-Length tidak valid: %d", content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_loader_error_t lerr = esp_loader_flash_start(&s_loader, target_offset,
                                                       content_len, DOWNLOAD_CHUNK_SIZE);
    if (lerr != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "esp_loader_flash_start gagal: %d", lerr);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t buf[DOWNLOAD_CHUNK_SIZE];
    int total_read = 0;

    while (total_read < content_len) {
        int to_read = content_len - total_read;
        if (to_read > DOWNLOAD_CHUNK_SIZE) {
            to_read = DOWNLOAD_CHUNK_SIZE;
        }

        int n = esp_http_client_read(client, (char *)buf, to_read);
        if (n <= 0) {
            ESP_LOGE(TAG, "HTTP read gagal/terputus di byte %d/%d", total_read, content_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        lerr = esp_loader_flash_write(&s_loader, buf, n);
        if (lerr != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "esp_loader_flash_write gagal di byte %d: %d", total_read, lerr);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        total_read += n;
        if (total_read % (DOWNLOAD_CHUNK_SIZE * 20) == 0 || total_read == content_len) {
            ESP_LOGI(TAG, "Progress: %d / %d bytes (%.0f%%)",
                     total_read, content_len, (100.0f * total_read) / content_len);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Selesai flash 0x%x (%d bytes)", (unsigned int)target_offset, total_read);
    return ESP_OK;
}

esp_err_t c3_programmer_flash_from_urls(const char *bootloader_url,
                                         const char *partition_url,
                                         const char *app_url)
{
    ESP_LOGI(TAG, "Menghubungi ESP32-C3 lewat UART...");

    esp_loader_connect_args_t connect_args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = esp_loader_connect(&s_loader, &connect_args);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "Gagal connect ke C3 (cek wiring boot/reset/UART): %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Terhubung ke ESP32-C3");

    // Naikkan baudrate supaya flashing lebih cepat (opsional tapi disarankan)
    err = esp_loader_change_transmission_rate(&s_loader, 460800);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "Gagal naikkan baudrate, lanjut dengan %d", C3_UART_BAUD_RATE);
    }

    esp_err_t ret;

    ret = flash_one_binary_from_url(bootloader_url, C3_BOOTLOADER_OFFSET);
    if (ret != ESP_OK) return ret;

    ret = flash_one_binary_from_url(partition_url, C3_PARTITION_OFFSET);
    if (ret != ESP_OK) return ret;

    ret = flash_one_binary_from_url(app_url, C3_APP_OFFSET);
    if (ret != ESP_OK) return ret;

    // Keluar dari mode download, C3 boot normal menjalankan firmware baru
    esp_loader_flash_finish(&s_loader, true);

    ESP_LOGI(TAG, "ESP32-C3 selesai diprogram & sedang restart ke firmware baru");
    return ESP_OK;
}