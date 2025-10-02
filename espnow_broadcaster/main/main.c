#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"   // wifi_tx_info_t
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"        // esp_timer_get_time()
#include "esp_now.h"

static const char* TAG = "ESP_NOW_BROADCASTER";

// ส่งให้ทุกคน (broadcast)
static uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// โครงสร้างข้อมูล (packed กัน padding)
typedef struct __attribute__((packed)) {
    char     sender_id[20];
    char     message[180];
    uint8_t  message_type;    // 1=Info, 2=Command, 3=Alert
    uint8_t  group_id;        // 0=All, 1=Group1, 2=Group2
    uint32_t sequence_num;
    uint64_t timestamp_ms;    // ใช้ 64-bit ป้องกัน overflow
} broadcast_data_t;

static uint32_t sequence_counter = 0;

/* ========== Callbacks (สเปกใหม่ v5.5) ========== */

// send-cb: ใช้ wifi_tx_info_t*
static void on_data_sent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    const char *res = (status == ESP_NOW_SEND_SUCCESS) ? "SUCCESS" : "FAIL";
    if (info) {
        ESP_LOGI(TAG, "Send CB: ifidx=%d -> %s", (int)info->ifidx, res);
    } else {
        ESP_LOGI(TAG, "Send CB: info=NULL -> %s", res);
    }
}

// recv-cb: ใช้ esp_now_recv_info_t*
static void on_data_recv(const esp_now_recv_info_t *recv_info,
                         const uint8_t *data, int len) {
    if (!recv_info) return;
    const uint8_t *src = recv_info->src_addr;

    ESP_LOGI(TAG, "📥 Reply from %02X:%02X:%02X:%02X:%02X:%02X, len=%d",
             src[0], src[1], src[2], src[3], src[4], src[5], len);

    if (len < (int)sizeof(broadcast_data_t)) {
        ESP_LOGW(TAG, "Payload too short (%d < %d), ignore",
                 len, (int)sizeof(broadcast_data_t));
        return;
    }

    broadcast_data_t reply;
    memcpy(&reply, data, sizeof(reply));
    reply.message[sizeof(reply.message)-1] = '\0';

    ESP_LOGI(TAG, "   Reply: \"%s\"  type=%u group=%u seq=%u t=%" PRIu64 "ms",
             reply.message, reply.message_type, reply.group_id,
             reply.sequence_num, reply.timestamp_ms);
}

/* ========== Wi-Fi & ESPNOW init ========== */

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ลด latency ของ ESP-NOW
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // ใช้โหมด STA สำหรับ ESP-NOW
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // ถ้าไม่ได้ต่อ AP เดียวกัน: อาจต้องล็อก channel ให้ตรงกันสองฝั่ง
    // ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi initialized & started");
}

static void init_espnow(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // เพิ่ม/อัปเดต peer เป็น Broadcast
    esp_now_peer_info_t peer_info;
    memset(&peer_info, 0, sizeof(peer_info));
    memcpy(peer_info.peer_addr, broadcast_mac, 6);
    peer_info.channel = 0;           // 0 = ใช้ channel ปัจจุบัน
    peer_info.ifidx   = WIFI_IF_STA; // จำเป็นใน v5.x
    peer_info.encrypt = false;

    if (esp_now_is_peer_exist(broadcast_mac)) {
        ESP_ERROR_CHECK(esp_now_mod_peer(&peer_info));
    } else {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
    }

    ESP_LOGI(TAG, "ESP-NOW Broadcasting initialized");
}

/* ========== ส่ง Broadcast ========== */

static void send_broadcast(const char* message, uint8_t msg_type, uint8_t group_id) {
    broadcast_data_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    // เตรียม payload อย่างปลอดภัย
    snprintf(pkt.sender_id,  sizeof(pkt.sender_id),  "%s", "MASTER_001");
    snprintf(pkt.message,    sizeof(pkt.message),    "%s", message);
    pkt.message_type = msg_type;
    pkt.group_id     = group_id;
    pkt.sequence_num = ++sequence_counter;
    pkt.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    ESP_LOGI(TAG, "📡 Broadcasting [type=%u, group=%u]: %s", msg_type, group_id, pkt.message);

    esp_err_t res = esp_now_send(broadcast_mac, (const uint8_t *)&pkt, sizeof(pkt));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send broadcast: %s", esp_err_to_name(res));
    }
}

/* ========== app_main ========== */

void app_main(void) {
    // NVS init (ตามแนวทาง IDF)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init();
    init_espnow();

    // แสดง MAC ตัวเอง
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "📍 Broadcaster MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "🚀 ESP-NOW Broadcaster started");

    int count = 0;
    while (1) {
        switch (count % 4) {
            case 0: send_broadcast("General announcement to all devices", 1, 0); break; // Info → All
            case 1: send_broadcast("Command for Group 1 devices",        2, 1); break; // Command → G1
            case 2: send_broadcast("Alert for Group 2 devices",          3, 2); break; // Alert → G2
            case 3: send_broadcast("Status update for all groups",       1, 0); break; // Info → All
        }
        count++;
        vTaskDelay(pdMS_TO_TICKS(5000)); // ส่งทุก 5 วินาที
    }
}