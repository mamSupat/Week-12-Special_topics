#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
    #include "esp_timer.h"  

static const char* TAG = "ESP_NOW_RECEIVER";

/* === ปรับให้ตรงกับโปรเจกต์ของคุณ === */
#define CHANNEL      1            // ★ ให้ตรงกับฝั่งส่ง
#define MY_NODE_ID   "NODE_003"
#define MY_GROUP_ID  1            // ★ ตั้งเป็น 1 ตามตัวอย่างที่อยากได้

/* payload ต้องเหมือนฝั่ง Broadcaster */
typedef struct __attribute__((packed)) {
    char     sender_id[20];
    char     message[180];
    uint8_t  message_type;   // 1=Info, 2=Command, 3=Alert
    uint8_t  group_id;       // 0=All, 1=Group1, 2=Group2
    uint32_t sequence_num;
    uint32_t timestamp_ms;
} broadcast_data_t;

/* ---------- utils ---------- */
static void log_mac(const char *pfx, const uint8_t mac[6]) {
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             pfx, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------- Wi-Fi (STA + lock channel) ---------- */
static void wifi_init_for_espnow(uint8_t ch) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (ch >= 1 && ch <= 13) {
        wifi_config_t sta = {0};
        sta.sta.channel = ch;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi STA started (channel=%u)", ch);
}

/* ---------- Send callback ---------- */
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Send status: %s", status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

/* กันรับซ้ำ */
static uint32_t s_last_seq = 0;

/* ส่ง reply/ACK กลับผู้ส่ง */
static void send_reply_to(const uint8_t dst_mac[6], const char *reply_text) {
    // เพิ่ม peer หากยังไม่มี
    if (!esp_now_is_peer_exist(dst_mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, dst_mac, 6);
        peer.ifidx   = WIFI_IF_STA;
        peer.channel = CHANNEL;
        peer.encrypt = false;
        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGE(TAG, "add peer failed (reply)");
            return;
        }
    }

    broadcast_data_t ack = (broadcast_data_t){0};
    snprintf(ack.sender_id, sizeof(ack.sender_id), "%s", MY_NODE_ID);
    snprintf(ack.message,   sizeof(ack.message),   "%s", reply_text);
    ack.message_type = 1; // INFO
    ack.group_id     = MY_GROUP_ID;
    ack.sequence_num = 0; // reply ไม่จำเป็นต้องใช้ seq
    ack.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    ESP_LOGI(TAG, "📤 Sending reply: %s", reply_text);
    esp_err_t er = esp_now_send(dst_mac, (const uint8_t*)&ack, sizeof(ack));
    if (er != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send(reply) failed: %s", esp_err_to_name(er));
    }
}

/* ---------- Receive callback (IDF v5.x signature) ---------- */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    broadcast_data_t rx = {0};
    memcpy(&rx, data, len < (int)sizeof(rx) ? len : (int)sizeof(rx));

    // แสดง MAC ผู้ส่ง “เหมือนเดิม”
    if (info && info->src_addr) {
        log_mac("📥 From", info->src_addr);
    }

    // กันข้อความซ้ำ (ตาม seq > 0)
    if (rx.sequence_num != 0 && rx.sequence_num <= s_last_seq) {
        ESP_LOGW(TAG, "⚠️  Duplicate ignored (seq=%" PRIu32 ")", rx.sequence_num);
        return;
    }
    if (rx.sequence_num > 0) s_last_seq = rx.sequence_num;

    // กรองกลุ่ม: รับเมื่อ group==0 (All) หรือ == MY_GROUP_ID
    if (rx.group_id != 0 && rx.group_id != MY_GROUP_ID) {
        // ถ้าอยากให้เงียบ ไม่ต้องพิมพ์อะไร ก็ลบบรรทัดนี้ได้
        ESP_LOGI(TAG, "Skip group=%u (mine=%u)", rx.group_id, MY_GROUP_ID);
        return;
    }

    const char *type_str = "UNKNOWN";
    if      (rx.message_type == 1) type_str = "INFO";
    else if (rx.message_type == 2) type_str = "COMMAND";
    else if (rx.message_type == 3) type_str = "ALERT";

    // ✅ รูปแบบข้อความตามที่คุณต้องการ
    ESP_LOGI(TAG, "📥 Received from %s:", rx.sender_id);
    ESP_LOGI(TAG, "   📨 Message: %s", rx.message);
    ESP_LOGI(TAG, "   🏷️ Type: %s", type_str);
    ESP_LOGI(TAG, "   👥 Group: %u", rx.group_id);

    // ถ้าเป็น COMMAND → ประมวลผล + ส่ง reply
    if (rx.message_type == 2) {
        ESP_LOGI(TAG, "🔧 Processing command...");
        if (info && info->src_addr) {
            send_reply_to(info->src_addr, "Command received and processed");
        }
    }

    ESP_LOGI(TAG, "--------------------------------");
}

/* ---------- ESP-NOW init ---------- */
static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_LOGI(TAG, "ESP-NOW Receiver ready");
}

/* ---------- app_main ---------- */
void app_main(void) {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_for_espnow(CHANNEL);
    espnow_init();

    // แสดงข้อมูลตัวเอง
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "📍 Node ID  : %s", MY_NODE_ID);
    ESP_LOGI(TAG, "📍 Group ID : %d", MY_GROUP_ID);
    ESP_LOGI(TAG, "📍 My MAC   : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}