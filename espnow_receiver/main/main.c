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

static const char* TAG = "ESP_NOW_DEVICE_B";

/* ★★ ใส่ MAC ของ Device A (STA) ★★ */
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

static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Reply status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

/* B: รับแล้วตอบกลับไปยังต้นทาง */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    const uint8_t *src = (info && info->src_addr) ? info->src_addr : NULL;
    if (src) log_mac("📥 Received from", src);
    ESP_LOGI(TAG, "📥 Recv len=%d", len);

    bidirectional_data_t rx = {0};
    memcpy(&rx, data, len < (int)sizeof(rx) ? len : (int)sizeof(rx));

    ESP_LOGI(TAG, "   👤 Device   : %s", rx.device_name);
    ESP_LOGI(TAG, "   💬 Message  : %s", rx.message);
    ESP_LOGI(TAG, "   🔢 Counter  : %d", rx.counter);
    ESP_LOGI(TAG, "   ⏰ Timestamp: %u ms", (unsigned)rx.timestamp_ms);

    if (!src) return;

    // ถ้ายังไม่มี peer ของต้นทาง ให้เพิ่มแบบไดนามิก
    if (!esp_now_is_peer_exist(src)) {
        esp_now_peer_info_t p = {0};
        memcpy(p.peer_addr, src, 6);
        p.ifidx   = WIFI_IF_STA;
        p.channel = 1;            // ★ ให้ตรงกับ CHANNEL
        p.encrypt = false;
        esp_err_t er = esp_now_add_peer(&p);
        if (er != ESP_OK && er != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(TAG, "add_peer(src) failed: %d", er);
            return;
        }
    }

    // สร้าง reply
    bidirectional_data_t tx = {0};
    strcpy(tx.device_name, "Device_B");
    snprintf(tx.message, sizeof(tx.message), "Reply to #%d - Thanks!", rx.counter);
    tx.counter      = rx.counter;
    tx.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    vTaskDelay(pdMS_TO_TICKS(100)); // กันชน timing เล็กน้อย
    esp_err_t er = esp_now_send(src, (const uint8_t *)&tx, sizeof(tx));
    if (er != ESP_OK) ESP_LOGE(TAG, "esp_now_send(reply) failed: %d", er);
    else ESP_LOGI(TAG, "📤 Replied to sender");
}

static void wifi_init_for_espnow(uint8_t channel) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (channel >= 1 && channel <= 13) {
        wifi_config_t sta_cfg = {0};
        sta_cfg.sta.channel = channel;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", channel);
}

static void espnow_init_and_add_partner(uint8_t channel) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // (ทางเลือก) เพิ่ม peer ของ A ล่วงหน้า — ไม่จำเป็นถ้ามี dynamic add ด้านบนแล้ว
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);   // ★ MAC ของ A
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;
    esp_err_t er = esp_now_add_peer(&peer);
    if (er != ESP_OK && er != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "add_peer(partner) warn: %d (จะอาศัย dynamic add ก็ได้)", er);
    } else {
        ESP_LOGI(TAG, "Peer(A) added (pre-registered)");
    }
}

void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const uint8_t CHANNEL = 1;   // ★★ ให้ตรงกับ Device A ★★
    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_partner(CHANNEL);

    // แสดง MAC ตัวเอง
    uint8_t mymac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mymac));
    log_mac("📍 My MAC:", mymac);

    ESP_LOGI(TAG, "ESP-NOW Device B ready (auto-reply)...");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}