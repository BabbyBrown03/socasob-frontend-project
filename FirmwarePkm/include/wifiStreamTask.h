#ifndef WIFI_STREAM_TASK_H_
#define WIFI_STREAM_TASK_H_

#include <stdint.h>
#include "connect_wifi.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"


// === Konfigurasi server TCP (ganti IP sesuai server Anda) ===
#define SERVER_HOST "10.45.173.156" // 192.168.1.21 atau 10.81.91.156
#define SERVER_PORT 3001
#define RECONNECT_DELAY_MS 1000
#define SOCKET_SEND_TIMEOUT_SEC 5
#define HTTP_STACK_SIZE (8 * 1024)

#define IS_WIFI_CONNECTED_BIT (1 << 0)
#define IS_STREAMING_BIT (1 << 1)
#define IS_TCP_CONNECTED (1 << 2)


int send_all(int sock, const uint8_t *data, size_t len);
void vTaskTcpStream(void *pvParameters);
void vTaskWifiConnect(void *pvParameter);

#endif