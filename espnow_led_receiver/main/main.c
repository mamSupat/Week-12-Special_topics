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
#include "driver/ledc.h"     // LEDC PWM

static const char* TAG = "ESP_NOW_LED_RX";

/* ★★ ใส่ STA MAC ของ “ฝั่ง A (รีโมต)” ★★ */
static uint8_t partner_mac[6] = {0x3C, 0x8A, 0x1F, 0x5D, 0x1B, 0x1C};

#define CHANNEL          1      // ★★ ให้ตรงกับฝั่ง A ★★
#define LED_PIN          2      // ★★ เปลี่ยนให้ตรงกับบอร์ดคุณ
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_CHANNEL     LEDC_CHANNEL_0
#define LEDC_BITS        LEDC_TIMER_8_BIT   // 8 บิต = 0..255
#define LEDC_FREQ_HZ     5000

typedef struct __attribute__((packed)) {
    bool     led_state;
    uint8_t  brightness;       // 0..255
    char     command[20];      // "SET_LED" / "LED_ACK"
} led_control_t;

static inline bool mac_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

static void log_mac(const char *prefix, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ตั้งค่า LEDC (PWM) */
static void led_pwm_init(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_BITS,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LED_PIN,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

/* ตั้งค่า LED ตามคำสั่ง */
static void led_apply(bool on, uint8_t bright) {
    uint32_t duty = on ? bright : 0; // 0..255
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
    ESP_LOGI(TAG, "LED %s, duty=%u/255", on ? "ON" : "OFF", (unsigned)duty);
}

/* ==== SEND-CB (log สถานะส่ง ACK) ==== */
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "ACK send status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

/* ==== RECV-CB (รับคำสั่งจากฝั่ง A และตอบกลับ) ==== */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !info->src_addr || !data || len <= 0) return;

    // รับเฉพาะจาก partner เท่านั้น (กันสัญญาณคนนอก)
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

    led_control_t cmd = {0};
    memcpy(&cmd, data, sizeof(cmd));

    if (strncmp(cmd.command, "SET_LED", sizeof(cmd.command)) == 0) {
        ESP_LOGI(TAG, "📥 SET_LED: state=%s, bright=%u",
                 cmd.led_state ? "ON" : "OFF", (unsigned)cmd.brightness);

        // ใช้งานจริง: clamp ความสว่าง
        if (cmd.brightness > 255) cmd.brightness = 255;
        led_apply(cmd.led_state, cmd.brightness);

        // เตรียมส่ง ACK กลับ
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t p = {0};
            memcpy(p.peer_addr, info->src_addr, 6);
            p.ifidx   = WIFI_IF_STA;
            p.channel = CHANNEL;
            p.encrypt = false;
            esp_err_t er = esp_now_add_peer(&p);
            if (er != ESP_OK && er != ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGE(TAG, "add_peer(A) failed: %d", er);
                return;
            }
        }

        led_control_t ack = {0};
        ack.led_state  = cmd.led_state;
        ack.brightness = cmd.brightness;
        snprintf(ack.command, sizeof(ack.command), "LED_ACK");

        esp_err_t er = esp_now_send(info->src_addr, (const uint8_t*)&ack, sizeof(ack));
        if (er != ESP_OK) ESP_LOGE(TAG, "esp_now_send(ACK) failed: %d", er);
        else ESP_LOGI(TAG, "📤 ACK sent");
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd.command);
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

static void espnow_init_and_add_partner(uint8_t channel) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // เพิ่ม peer ของฝั่ง A ไว้ล่วงหน้า (ทางเลือก — มี dynamic add ใน callback อยู่แล้ว)
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;
    esp_err_t er = esp_now_add_peer(&peer);
    if (er == ESP_OK || er == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(TAG, "Peer(A) pre-added");
    } else {
        ESP_LOGW(TAG, "add_peer(partner) warn: %d (จะอาศัย dynamic add ก็ได้)", er);
    }
}

void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_partner(CHANNEL);
    led_pwm_init();

    // พิมพ์ MAC ตัวเองช่วยตั้งค่า
    uint8_t mymac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mymac));
    log_mac("📍 My STA MAC:", mymac);

    ESP_LOGI(TAG, "LED Controller ready (pin=%d, 8-bit PWM)", LED_PIN);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}