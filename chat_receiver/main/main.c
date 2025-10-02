// main/espnow_chat_b.c
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>   // ★ ใช้ PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"

#define DEVICE_NAME "ESP32_B"
static const char *TAG = "ESP_NOW_CHAT_B";

/* ★★ ใส่ MAC ของ “ฝั่ง A” ให้ถูกต้อง ★★ */
static uint8_t partner_mac[6] = {0x3C, 0x8A, 0x1F, 0x5D, 0x1B, 0x1C};

#define CHANNEL 1

typedef struct __attribute__((packed)) {
    char     sender_name[20];
    char     message[200];
    uint32_t msg_id;
    bool     is_ack;
} chat_message_t;

static void log_mac(const char *pfx, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             pfx, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static volatile uint32_t g_last_sent_id = 0;
static volatile bool     g_last_sent_ack = false;
static uint32_t          g_counter = 0;

static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    if (info && info->src_addr) log_mac("📥 From", info->src_addr);

    chat_message_t rx = {0};
    int n = len < (int)sizeof(rx) ? len : (int)sizeof(rx);
    memcpy(&rx, data, n);

    if (rx.is_ack) {
        ESP_LOGI(TAG, "✅ ACK for msg_id=%" PRIu32 " from %s", rx.msg_id, rx.sender_name);
        if (rx.msg_id == g_last_sent_id) g_last_sent_ack = true;
        return;
    }

    ESP_LOGI(TAG, "💬 %s (#%" PRIu32 "): %s", rx.sender_name, rx.msg_id, rx.message);

    chat_message_t ack = {0};
    snprintf(ack.sender_name, sizeof(ack.sender_name), "%s", DEVICE_NAME);
    snprintf(ack.message, sizeof(ack.message), "ACK:%" PRIu32, rx.msg_id);
    ack.msg_id = rx.msg_id;
    ack.is_ack = true;

    const uint8_t *dst = (info && info->src_addr) ? info->src_addr : partner_mac;
    esp_err_t er = esp_now_send(dst, (const uint8_t *)&ack, sizeof(ack));
    if (er != ESP_OK) ESP_LOGE(TAG, "send ACK failed: %s", esp_err_to_name(er));
}

static void wifi_init_for_espnow(uint8_t ch) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (ch >= 1 && ch <= 13) {
        wifi_config_t sta = {0};
        sta.sta.channel = ch;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", ch);
}

static void espnow_init_and_add_peer(uint8_t ch) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = ch;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "ESP-NOW ok & peer added");
}

static void send_chat(const char *text) {
    chat_message_t tx = {0};
    snprintf(tx.sender_name, sizeof(tx.sender_name), "%s", DEVICE_NAME);
    snprintf(tx.message, sizeof(tx.message), "%s", text);
    tx.msg_id = ++g_counter;
    tx.is_ack = false;

    g_last_sent_id = tx.msg_id;
    g_last_sent_ack = false;

    ESP_LOGI(TAG, "📤 Send #%" PRIu32 ": %s", tx.msg_id, tx.message);
    esp_err_t er = esp_now_send(partner_mac, (const uint8_t *)&tx, sizeof(tx));
    if (er != ESP_OK) ESP_LOGE(TAG, "esp_now_send: %s", esp_err_to_name(er));
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_peer(CHANNEL);

    uint8_t my[6];
    esp_wifi_get_mac(WIFI_IF_STA, my);
    log_mac("📍 My MAC:", my);
    ESP_LOGI(TAG, "Chat name: %s", DEVICE_NAME);

    while (1) {
        char text[80];
        uint32_t ms = (uint32_t)(esp_timer_get_time()/1000ULL);
        snprintf(text, sizeof(text), "Hi from %s! Time=%" PRIu32 " ms", DEVICE_NAME, ms);
        send_chat(text);

        int wait = 0;
        while (!g_last_sent_ack && wait < 20) { // ~2s
            vTaskDelay(pdMS_TO_TICKS(100));
            wait++;
        }
        if (!g_last_sent_ack) {
            ESP_LOGW(TAG, "No ACK for msg_id=%" PRIu32, g_last_sent_id);
        }

        vTaskDelay(pdMS_TO_TICKS(7000));
    }
}