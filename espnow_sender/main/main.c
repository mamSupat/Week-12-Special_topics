#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_timer.h"

static const char* TAG = "ESP_NOW_DEVICE_A";

/* ★★ ใส่ MAC ของ Device B (STA) ★★
   ตัวอย่าง: 94:B5:55:F8:5D:50 -> {0x94,0xB5,0x55,0xF8,0x5D,0x50} */
static uint8_t partner_mac[6] = {0x3C, 0x8A, 0x1F, 0x5D, 0x1B, 0x1C};

typedef struct {
    char     device_name[50];
    char     message[150];
    int      counter;
    uint32_t timestamp_ms;
} bidirectional_data_t;

static void log_mac(const char *prefix, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* send-cb (รูปแบบใหม่ v5.x) */
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

/* recv-cb (รูปแบบใหม่ v5.x) : A แค่รับ reply และ log — ไม่ตอบกลับซ้ำ */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    if (info && info->src_addr) log_mac("📥 Reply from", info->src_addr);

    bidirectional_data_t rx = {0};
    memcpy(&rx, data, len < (int)sizeof(rx) ? len : (int)sizeof(rx));

    ESP_LOGI(TAG, "   💬 Message : %s", rx.message);
    ESP_LOGI(TAG, "   🔢 Counter : %d", rx.counter);
    ESP_LOGI(TAG, "   ⏰ Timestamp(ms): %u", (unsigned)rx.timestamp_ms);
}

static void wifi_init_for_espnow(uint8_t channel) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (channel >= 1 && channel <= 13) {
        wifi_config_t sta_cfg = {0};
        sta_cfg.sta.channel = channel;   // ★ ให้ตรงกับอีกเครื่อง
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", channel);
}

static void espnow_init_and_add_peer(uint8_t channel) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);   // ★ MAC ของ B
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;

    esp_err_t er = esp_now_add_peer(&peer);
    if (er != ESP_OK && er != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %d", er);
        ESP_ERROR_CHECK(er);
    }
    ESP_LOGI(TAG, "Peer(B) added");
}

void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const uint8_t CHANNEL = 1;  // ★★ ให้ตรงกับ Device B ★★
    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_peer(CHANNEL);

    // แสดง MAC ตัวเอง
    uint8_t mymac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mymac));
    log_mac("📍 My MAC:", mymac);

    // ส่งทุก 5 วินาที
    bidirectional_data_t tx = {0};
    int counter = 0;
    while (1) {
        strcpy(tx.device_name, "Device_A");
        snprintf(tx.message, sizeof(tx.message), "Hello! This is message number %d", counter);
        tx.counter      = counter++;
        tx.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        ESP_LOGI(TAG, "📤 Sending message #%d", tx.counter);
        esp_err_t er = esp_now_send(partner_mac, (const uint8_t *)&tx, sizeof(tx));
        if (er != ESP_OK) ESP_LOGE(TAG, "esp_now_send failed: %d", er);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}