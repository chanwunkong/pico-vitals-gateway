#ifndef MODE_BLE_RECEIVE_H
#define MODE_BLE_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    // 累積 idle_timeout_ms 毫秒沒有新資料、且待傳佇列非空（一般是等待逾時，
    // 也可能是 KEY1 手動要求立即上傳），呼叫端應切換到上傳模式。
    MODE_BLE_RECEIVE_EXIT_UPLOAD = 0,
    // KEY0 長按觸發，呼叫端應切換到熱點設定模式。
    MODE_BLE_RECEIVE_EXIT_ENTER_CONFIG,
} mode_ble_receive_exit_t;

// BLE 接收模式：持續掃描/連線/接收 FORA 裝置資料並寫入 storage。
mode_ble_receive_exit_t mode_ble_receive_run(uint32_t idle_timeout_ms);

#endif // MODE_BLE_RECEIVE_H
