#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"

static const char* TAG = "ESP_NOW_LED_TX";

/* ★★ ใส่ STA MAC ของ “ฝั่ง B (ตัวควบคุม LED)” ★★
   ตัวอย่าง 94:B5:55:F8:5D:50 -> {0x94,0xB5,0x55,0xF8,0x5D,0x50} */
static uint8_t partner_mac[6] = {0x3C, 0x8A, 0x1F, 0x5D, 0x1B, 0x1C};

#define SEND_PERIOD_MS   3000
#define CHANNEL          1       // ★★ ให้ตรงกับอีกเครื่อง

/* Payload คำสั่งควบคุม LED (packed กันเพี้ยน) */
typedef struct __attribute__((packed)) {
    bool     led_state;          // true = ON
    uint8_t  brightness;         // 0..255
    char     command[20];        // "SET_LED" / "LED_ACK"
} led_control_t;

/* เทียบ MAC แบบสั้น */
static inline bool mac_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

static void log_mac(const char *prefix, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ==== SEND-CB (รูปแบบใหม่ v5.x) ==== */
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

/* ==== RECV-CB (รับ ACK กลับ) ==== */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !info->src_addr || !data || len <= 0) return;

    // รับเฉพาะจาก partner เท่านั้น
    if (!mac_eq(info->src_addr, partner_mac)) {
        ESP_LOGW(TAG, "Ignore from %02X:%02X:%02X:%02X:%02X:%02X len=%d",
                 info->src_addr[0],info->src_addr[1],info->src_addr[2],
                 info->src_addr[3],info->src_addr[4],info->src_addr[5], len);
        return;
    }

    if (len != sizeof(led_control_t)) {
        ESP_LOGW(TAG, "Size mismatch: %d != %d", len, (int)sizeof(led_control_t));
        return;
    }

    led_control_t rx = {0};
    memcpy(&rx, data, sizeof(rx));
    if (strncmp(rx.command, "LED_ACK", sizeof(rx.command)) == 0) {
        log_mac("📥 ACK from", info->src_addr);
        ESP_LOGI(TAG, "   LED: %s, Brightness: %u",
                 rx.led_state ? "ON" : "OFF", (unsigned)rx.brightness);
    } else {
        ESP_LOGW(TAG, "Unknown command in reply: %s", rx.command);
    }
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

static void espnow_init_and_add_peer(uint8_t channel) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;
    esp_err_t er = esp_now_add_peer(&peer);
    if (er != ESP_OK && er != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add_peer failed: %d", er);
        ESP_ERROR_CHECK(er);
    }
    ESP_LOGI(TAG, "Peer added");
}

void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_peer(CHANNEL);

    // พิมพ์ MAC ตัวเอง (ช่วยตั้งค่า)
    uint8_t mymac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mymac));
    log_mac("📍 My STA MAC:", mymac);

    // ส่งคำสั่งสลับ ON/OFF + ปรับความสว่าง demo
    bool state = true;
    uint8_t brightness = 64;
    while (1) {
        led_control_t cmd = {0};
        cmd.led_state  = state;
        cmd.brightness = brightness;
        snprintf(cmd.command, sizeof(cmd.command), "SET_LED");

        ESP_LOGI(TAG, "📤 SET_LED: state=%s, bright=%u", state ? "ON" : "OFF", (unsigned)brightness);
        esp_err_t er = esp_now_send(partner_mac, (const uint8_t*)&cmd, sizeof(cmd));
        if (er != ESP_OK) ESP_LOGE(TAG, "esp_now_send failed: %d", er);

        // เดโม่: ไล่สว่าง + toggle
        brightness += 64; // 64 -> 128 -> 192 -> 0 (overflow wrap)
        if (brightness < 64) brightness = 0;  // เมื่อ overflow ให้เป็น 0
        state = !state;

        vTaskDelay(pdMS_TO_TICKS(SEND_PERIOD_MS));
    }
}