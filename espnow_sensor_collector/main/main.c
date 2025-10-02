// main/espnow_sensor_rx.c
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"

static const char* TAG = "ESP_NOW_SENSOR_RX";

/* โครงสร้างต้องเหมือนฝั่งส่งทุก byte */
typedef struct __attribute__((packed)) {
    float    temperature;
    float    humidity;
    int32_t  light_level;
    char     sensor_id[10];
    uint32_t timestamp_ms;
} sensor_data_t;

/* พิมพ์ MAC ให้อ่านง่าย */
static void log_mac(const char *prefix, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Wi-Fi STA + (ถ้าต้องการ) ล็อกชานเนลให้ตรงกับฝั่งส่ง */
static void wifi_init_for_espnow(uint8_t channel /*1..13 หรือ 0=ไม่ล็อก*/) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (channel >= 1 && channel <= 13) {
        wifi_config_t sta_cfg = {0};
        sta_cfg.sta.channel = channel;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", channel);
}

/* recv callback (รูปแบบใหม่ v5.x) */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    if (info && info->src_addr) {
        log_mac("📥 From", info->src_addr);
    }

    if (len != sizeof(sensor_data_t)) {
        ESP_LOGW(TAG, "size mismatch: %d (expect %u)", len, (unsigned)sizeof(sensor_data_t));
        return;
    }

    sensor_data_t rx = {0};
    memcpy(&rx, data, sizeof(rx));

    ESP_LOGI(TAG, "   ID   : %s", rx.sensor_id);
    ESP_LOGI(TAG, "   Temp : %.2f C", rx.temperature);
    ESP_LOGI(TAG, "   Hum  : %.2f %%", rx.humidity);
    ESP_LOGI(TAG, "   LDR  : %ld", (long)rx.light_level);
    ESP_LOGI(TAG, "   Time : %" PRIu32 " ms", rx.timestamp_ms);
    ESP_LOGI(TAG, "--------------------------------");
}

void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const uint8_t CHANNEL = 1;   // ★★ ตั้งเท่ากับฝั่ง TX ★★
    wifi_init_for_espnow(CHANNEL);

    // แสดง MAC ตัวเอง (คัดลอกไปใส่ partner_mac ในฝั่ง TX)
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    log_mac("📍 My MAC:", mac);

    // เริ่ม ESP-NOW + ลงทะเบียน callback
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_LOGI(TAG, "ESP-NOW RX ready…");

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}