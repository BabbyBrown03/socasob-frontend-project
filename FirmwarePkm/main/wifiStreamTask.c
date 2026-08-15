#include "wifiStreamTask.h"
#include "driver/uart.h"

// =============================================================================
// === TCP Client (plain socket, length-prefix framing) ===
// =============================================================================
//
// Format pengiriman tiap frame:
//   [4 byte big-endian: panjang JPEG] [data JPEG sepanjang itu]
//
// Server harus baca 4 byte dulu untuk tahu berapa banyak byte berikutnya
// yang merupakan 1 frame JPEG utuh.

extern QueueHandle_t frame_queue;
extern camera_fb_t *fb;
extern EventGroupHandle_t wifiEventGroup;
static const char *TAG_TCP = "TCP";


static int g_sock = -1; // socket TCP aktif (-1 jika belum/ tidak terhubung)

// ---------------------------------------------------------------------------
// Status helpers (thread-safe)
// ---------------------------------------------------------------------------



static int tcp_connect_to_server(void)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_HOST);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG_TCP, "Gagal membuat socket: errno %d", errno);
        return -1;
    }

    // Nonaktifkan Nagle's algorithm supaya data tidak ditahan/digabung,
    // penting untuk latency rendah pada pengiriman frame berurutan.
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Timeout kirim, supaya tidak block selamanya kalau koneksi macet
    struct timeval timeout;
    timeout.tv_sec = SOCKET_SEND_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG_TCP, "Menghubungkan ke %s:%d ...", SERVER_HOST, SERVER_PORT);
    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG_TCP, "Gagal connect: errno %d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG_TCP, "Terhubung ke server!");
    return sock;
}

// Kirim seluruh buffer (handle short write, TCP bisa kirim sebagian dulu)
int send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent_total = 0;
    while (sent_total < len) {
        int n = send(sock, data + sent_total, len - sent_total, 0);
        if (n < 0) {
            return -1; // error / timeout
        }
        sent_total += n;
    }
    return (int)sent_total;
}

// Task: ambil frame dari queue, kirim via TCP dengan length-prefix.
// Auto-reconnect jika socket terputus.
void vTaskTcpStream(void *pvParameters)
{
    camera_fb_t *fb = NULL;

    for (;;) {
        // Pastikan socket terhubung sebelum coba kirim
        if (g_sock < 0) {
            g_sock = tcp_connect_to_server();
            if (g_sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
                continue;
            }
        }

        if (xQueueReceive(frame_queue, &fb, pdMS_TO_TICKS(500)) == pdTRUE) {
            int64_t t0 = esp_timer_get_time();

            uint32_t len_be = htonl((uint32_t)fb->len);
            int ok1 = send_all(g_sock, (const uint8_t *)&len_be, sizeof(len_be));
            int ok2 = -1;
            if (ok1 >= 0) {
                ok2 = send_all(g_sock, fb->buf, fb->len);
            }

            int64_t dur_ms = (esp_timer_get_time() - t0) / 1000;

            if (ok1 >= 0 && ok2 >= 0) {
                if (dur_ms > 60) {
                    ESP_LOGW(TAG_TCP, "Kirim %u bytes butuh %lld ms",
                             (unsigned)fb->len, dur_ms);
                }

                // Read eye status from TCP server (non-blocking)
                char server_cmd;
                int recv_len = recv(g_sock, &server_cmd, 1, MSG_DONTWAIT);
                if (recv_len > 0) {
                    ESP_LOGI(TAG_TCP, "Received eye state from server: %c", server_cmd);
                    if (server_cmd == 'N' || server_cmd == 'F' || server_cmd == 'D') {
                        // Forward to ESP32-C3
                        uart_write_bytes(UART_NUM_1, &server_cmd, 1);
                    }
                }
            } else {
                ESP_LOGW(TAG_TCP, "Gagal kirim (errno %d), reconnect...", errno);
                close(g_sock);
                g_sock = -1;
                xEventGroupClearBits(wifiEventGroup, IS_TCP_CONNECTED);
            }

            esp_camera_fb_return(fb);
            fb = NULL;
        }
    }
}

void vTaskWifiConnect(void *pvParameter)
{
    while (connect_wifi() == ESP_FAIL) {
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI("WIFI", "WiFi Terhubung!");
    xEventGroupSetBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT);

    // Matikan power-save WiFi: modem sleep sering menyebabkan AP harus
    // rebuild Block-Ack session berulang kali (terlihat di log sebagai
    // ADDBA/DELBA churn), yang menjatuhkan throughput drastis saat streaming.
    esp_wifi_set_ps(WIFI_PS_NONE);

    xTaskCreatePinnedToCore(vTaskTcpStream, "taskTcpStream", HTTP_STACK_SIZE, NULL, 15, NULL, 0);

    vTaskDelete(NULL);
}

