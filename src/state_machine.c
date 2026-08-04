#include "state_machine.h"

#include "led_status.h"
#include "mode_ap_config.h"
#include "mode_ble_receive.h"
#include "mode_boot_select.h"
#include "mode_upload.h"

#include "btstack.h"
#include "pico/cyw43_arch.h"

#include <stdio.h>

#define BOOT_CONFIG_WINDOW_MS      4000
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
    bool enter_config = mode_boot_select_check(BOOT_CONFIG_WINDOW_MS);
    gateway_state_t state = enter_config ? STATE_AP_CONFIG : STATE_BLE_RECEIVE;
    printf("[FSM] boot select window done, enter_config=%d\n", enter_config);

    for (;;) {
        switch (state) {
            case STATE_AP_CONFIG:
                printf("[FSM] -> AP_CONFIG\n");
                radio_switch_to_wifi();
                led_status_set(LED_SOLID_ON);
                mode_ap_config_run(); // 阻塞直到設定完成並儲存
                state = STATE_BLE_RECEIVE;
                break;

            case STATE_BLE_RECEIVE: {
                printf("[FSM] -> BLE_RECEIVE\n");
                radio_switch_to_bluetooth();
                bool idle_timeout = mode_ble_receive_run(BLE_IDLE_UPLOAD_TRIGGER_MS);
                state = idle_timeout ? STATE_UPLOAD : STATE_BLE_RECEIVE;
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
