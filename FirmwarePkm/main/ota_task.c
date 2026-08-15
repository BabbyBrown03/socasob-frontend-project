#include "ota_task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_OTA = "OTA";

// TODO: ganti dengan URL server firmware kamu.
// Untuk testing lokal: IP laptop yang menjalankan `python -m http.server 8000`
// di jaringan WiFi yang sama dengan ESP32.
#define OTA_URL "http://10.45.173.156:8000/firmwarePkm.bin"

// Interval pengecekan OTA otomatis (ms)
#define OTA_CHECK_INTERVAL_MS (60 * 60 * 1000) // 1 jam

void ota_confirm_running_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG_OTA, "Firmware pending verify -> konfirmasi valid, cancel rollback");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_LOGI(TAG_OTA, "Boot dari partisi: %s @ 0x%x", running->label, (unsigned int)running->address);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG_OTA, "Firmware version: %s | build: %s %s",
             app_desc->version, app_desc->date, app_desc->time);
}

esp_err_t start_ota_update(const char *url)
{
    ESP_LOGI(TAG_OTA, "Mulai OTA dari %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL, // ganti ke esp_crt_bundle_attach kalau pakai HTTPS + CA bundle
        .skip_cert_common_name_check = false,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_OTA, "esp_https_ota_begin gagal: %s", esp_err_to_name(ret));
        return ret;
    }

    // Bandingkan versi firmware baru vs yang sedang berjalan, supaya tidak
    // flash ulang firmware yang identik.
    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        const esp_app_desc_t *running_app_info = esp_app_get_description();
        ESP_LOGI(TAG_OTA, "Versi berjalan : %s", running_app_info->version);
        ESP_LOGI(TAG_OTA, "Versi baru     : %s", new_app_info.version);

        if (strcmp(new_app_info.version, running_app_info->version) == 0) {
            ESP_LOGW(TAG_OTA, "Versi sama, OTA dibatalkan");
            esp_https_ota_abort(ota_handle);
            return ESP_FAIL;
        }
    }

    // Loop download + tulis ke partisi OTA
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // ESP_LOGD(TAG_OTA, "Bytes read: %d", esp_https_ota_get_image_len_read(ota_handle));
    }

    if (ret != ESP_OK) {
        // Ini titik yang sebelumnya gagal dengan err=0x104 (ESP_ERR_INVALID_SIZE)
        // kalau file .bin lebih besar dari partisi OTA yang tersedia.
        ESP_LOGE(TAG_OTA, "esp_https_ota_perform gagal: 0x%x (%s)", ret, esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        return ret;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG_OTA, "Data firmware belum lengkap diterima");
        esp_https_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    esp_err_t finish_ret = esp_https_ota_finish(ota_handle);
    if (finish_ret != ESP_OK) {
        if (finish_ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG_OTA, "Image OTA gagal validasi (checksum/signature)");
        }
        ESP_LOGE(TAG_OTA, "OTA gagal: %s", esp_err_to_name(finish_ret));
        return finish_ret;
    }

    ESP_LOGI(TAG_OTA, "OTA berhasil, restart perangkat dalam 1 detik...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // beri waktu log terakhir terkirim (UDP/serial)
    esp_restart();

    return ESP_OK; // tidak akan pernah sampai sini
}

void vTaskOtaCheck(void *pvParameters)
{
    for (;;) {
        esp_err_t err = start_ota_update(OTA_URL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_OTA, "OTA check gagal (%s), coba lagi nanti", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}