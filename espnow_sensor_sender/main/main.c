// main/espnow_sensor_tx.c
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us()

static const char* TAG = "ESP_NOW_SENSOR_TX";

/* ★★ ตั้ง MAC ของ “ตัวรับ” (อีกบอร์ด) ให้ถูกต้องก่อนใช้งาน ★★
   ตัวอย่าง 94:B5:55:F8:5D:50 -> {0x94,0xB5,0x55,0xF8,0x5D,0x50} */
static uint8_t partner_mac[6] = {0x3C, 0x8A, 0x1F, 0x5D, 0x1B, 0x1C};

/* === กำหนดขาเซ็นเซอร์ ===
   DHT11: ต่อที่ GPIO4 (ปรับตามการต่อจริง)
   LDR: ใช้ ADC1_CHANNEL_6 = GPIO34 (ปรับตามการต่อจริง)
*/
#define DHT_PIN           GPIO_NUM_4
#define LDR_ADC_CHANNEL   ADC1_CHANNEL_6

/* โครงสร้าง payload (แพ็กเพื่อลดปัญหา alignment) */
typedef struct __attribute__((packed)) {
    float    temperature;
    float    humidity;
    int32_t  light_level;     // ค่า raw ADC (0..4095 ที่ความกว้าง 12 บิต)
    char     sensor_id[10];   // เช่น "TEMP_01"
    uint32_t timestamp_ms;    // esp_timer_get_time()/1000
} sensor_data_t;

/* ---------- ESP-NOW callbacks (v5.x) ---------- */
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s", (status == ESP_NOW_SEND_SUCCESS) ? "SUCCESS" : "FAIL");
}

/* ---------- Wi-Fi & ESP-NOW init ---------- */
static void wifi_init_for_espnow(uint8_t channel /*1..13 หรือ 0=ไม่ล็อก*/) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (channel >= 1 && channel <= 13) {
        wifi_config_t sta_cfg = {0};
        sta_cfg.sta.channel = channel;  // ★ ให้ตรงกับอีกบอร์ด
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", channel);
}

static void espnow_init_and_add_peer(uint8_t channel) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, partner_mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = channel;
    peer.encrypt = false;

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "ESP-NOW init OK & peer added");
}

/* ---------- DHT11 bit-bang อ่านค่าพื้นฐาน ---------- */
static bool wait_level(gpio_num_t pin, int level, uint32_t timeout_us) {
    uint64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < timeout_us) {
        if (gpio_get_level(pin) == level) return true;
    }
    return false;
}

/* อ่าน DHT11: คืน true ถ้าสำเร็จ และกรอกค่า temp/hum */
static bool dht11_read(gpio_num_t pin, float *temp, float *hum) {
    uint8_t data[5] = {0};

    // start signal: ดึงลง ~18ms
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    // response: low ~80us, high ~80us
    if (!wait_level(pin, 0, 100)) return false;
    if (!wait_level(pin, 1, 100)) return false;
    if (!wait_level(pin, 0, 100)) return false; // เตรียมเริ่มอ่านบิต

    // อ่าน 40 บิต (5 ไบต์)
    for (int i = 0; i < 40; i++) {
        if (!wait_level(pin, 1, 100)) return false; // ขอบขึ้น
        uint64_t start = esp_timer_get_time();
        if (!wait_level(pin, 0, 100)) return false; // รอให้ลง
        uint32_t high_us = (uint32_t)(esp_timer_get_time() - start);

        // 26~28us = 0, ~70us = 1 (ตั้ง threshold ประมาณ 50us)
        uint8_t bit = (high_us > 50) ? 1 : 0;

        data[i / 8] <<= 1;
        data[i / 8] |= bit;
    }

    uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) return false;

    // DHT11 ทศนิยมเป็น 0: RH=data[0], T=data[2]
    *hum  = (float)data[0];
    *temp = (float)data[2];
    return true;
}

/* ---------- LDR ADC (legacy driver) ---------- */
static void adc_init(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LDR_ADC_CHANNEL, ADC_ATTEN_DB_12);
}
static int read_light_raw(void) {
    int sum = 0, n = 8;
    for (int i = 0; i < n; ++i) {
        sum += adc1_get_raw(LDR_ADC_CHANNEL);
        esp_rom_delay_us(200);
    }
    return sum / n;
}

/* ---------- สุ่มค่าจำลอง เมื่ออ่าน DHT11 fail ---------- */
static void sample_sensor(float *t, float *h, int *ldr) {
    float tt, hh;
    if (dht11_read(DHT_PIN, &tt, &hh)) {
        *t = tt;
        *h = hh;
    } else {
        // fallback ทดสอบง่าย ๆ
        *t = 25.0f + (float)((int)(esp_random() % 200) - 100) / 100.0f; // ~24..26
        *h = 60.0f + (float)((int)(esp_random() % 200) - 100) / 100.0f; // ~59..61
    }
    *ldr = read_light_raw();
}

/* ---------- main ---------- */
void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const uint8_t CHANNEL = 1;  // ★★ ให้ตรงกันทั้งสองบอร์ด ★★
    wifi_init_for_espnow(CHANNEL);
    espnow_init_and_add_peer(CHANNEL);

    // แสดง MAC ตัวเอง
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "My MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // เตรียม GPIO/ADC
    gpio_set_pull_mode(DHT_PIN, GPIO_PULLUP_ONLY);
    adc_init();

    sensor_data_t pkt = {0};
    strcpy(pkt.sensor_id, "TEMP_01");

    while (1) {
        float t = 0, h = 0;
        int   l = 0;
        sample_sensor(&t, &h, &l);

        pkt.temperature  = t;
        pkt.humidity     = h;
        pkt.light_level  = l;
        pkt.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        ESP_LOGI(TAG, "TX -> T=%.2fC H=%.2f%% LDR=%d ts=%" PRIu32 "ms",
                 pkt.temperature, pkt.humidity, pkt.light_level, pkt.timestamp_ms);

        esp_err_t er = esp_now_send(partner_mac, (const uint8_t *)&pkt, sizeof(pkt));
        if (er != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(er));
        }

        vTaskDelay(pdMS_TO_TICKS(5000));  // ส่งทุก 5 วิ
    }
}