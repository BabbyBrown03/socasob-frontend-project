set(srcs "main.c"
         "connect_wifi.c"
         "wifiStreamTask.c"
         "cameraTask.c"
         "udpLogger.c"
         "ota_task.c"
         "c3_programmer.c"
         )
idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "." "../include"
                    REQUIRES "driver" "esp_lcd" "nvs_flash" "esp_wifi" "esp_http_client" "esp_timer" "app_update" "esp_https_ota" "esp_http_client" "nvs_flash" "lwip" "esp_wifi" "esp_http_server"
                    )