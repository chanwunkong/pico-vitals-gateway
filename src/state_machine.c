#include "state_machine.h"

#include "led_status.h"
#include "mode_ap_config.h"
#include "mode_ble_receive.h"
#include "mode_upload.h"

#include "btstack.h"
#include "pico/cyw43_arch.h"

#include <stdio.h>

// 開機後第一次進 AP_CONFIG 給的時間上限（2026-08-28 取代原本的 BOOTSEL 開機
// 視窗判斷）：過了這段時間都沒人送出設定表單、也沒按 KEY0 取消，就自動當作
// 「這次開機沒有人要設定」，放行到正常的 BLE_RECEIVE 監測模式，熱點不會無限
// 期擋住 24/7 監測。KEY0 長按手動重新進入這個模式（mode_ble_receive.c 的
// MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG）不套用這個逾時，使用者主動要求設定就
// 讓他填完，沿用原本「按住 KEY0 取消」的唯一離開方式。
#define AP_CONFIG_BOOT_TIMEOUT_MS  180000
#define BLE_IDLE_UPLOAD_TRIGGER_MS 5000

typedef enum {
    STATE_AP_CONFIG,
    STATE_BLE_RECEIVE,
    STATE_UPLOAD,
} gateway_state_t;

// 確保同一時間只有一種無線功能在跑：進入 WiFi 相關狀態前關閉藍芽，反之亦然。
// 此規則統一放在這裡把關，避免日後改動個別模式模組時被意外破壞。
static void radio_switch_to_bluetooth(void) {
    cyw43_arch_disable_ap_mode();
    cyw43_arch_disable_sta_mode();
    hci_power_control(HCI_POWER_ON);
}

static void radio_switch_to_wifi(void) {
    hci_power_control(HCI_POWER_OFF);
}

void state_machine_run(void) {
    // 每次通電一律先進 AP_CONFIG（含設定頁面），不再需要按住 BOOTSEL/KEY0
    // 才能進入；給 AP_CONFIG_BOOT_TIMEOUT_MS 的時間上限，逾時自動放行到
    // BLE_RECEIVE（見上面常數說明）。
    gateway_state_t state = STATE_AP_CONFIG;
    uint32_t ap_config_timeout_ms = AP_CONFIG_BOOT_TIMEOUT_MS;

    for (;;) {
        switch (state) {
            case STATE_AP_CONFIG:
                printf("[FSM] -> AP_CONFIG (timeout_ms=%u)\n", ap_config_timeout_ms);
                radio_switch_to_wifi();
                led_status_set(LED_SOLID_ON);
                mode_ap_config_run(ap_config_timeout_ms); // 阻塞直到設定完成/取消/逾時
                ap_config_timeout_ms = 0; // 之後的重新進入（KEY0 長按）不設逾時
                state = STATE_BLE_RECEIVE;
                break;

            case STATE_BLE_RECEIVE: {
                printf("[FSM] -> BLE_RECEIVE\n");
                radio_switch_to_bluetooth();
                mode_ble_receive_exit_t exit_reason = mode_ble_receive_run(BLE_IDLE_UPLOAD_TRIGGER_MS);
                state = (exit_reason == MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG) ? STATE_AP_CONFIG : STATE_UPLOAD;
                break;
            }

            case STATE_UPLOAD:
                printf("[FSM] -> UPLOAD\n");
                radio_switch_to_wifi();
                led_status_set(LED_UPLOAD_BLINK);
                mode_upload_run(); // 無論成功失敗都會返回
                state = STATE_BLE_RECEIVE;
                break;
        }
    }
}
