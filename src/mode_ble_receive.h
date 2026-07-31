#ifndef MODE_BLE_RECEIVE_H
#define MODE_BLE_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

// BLE 接收模式：持續掃描/連線/接收 FORA 裝置資料並寫入 storage。
// 若連續 idle_timeout_ms 毫秒沒有收到新資料，回傳 true
//（呼叫端 state_machine 應切換到上傳模式）。
bool mode_ble_receive_run(uint32_t idle_timeout_ms);

#endif // MODE_BLE_RECEIVE_H
