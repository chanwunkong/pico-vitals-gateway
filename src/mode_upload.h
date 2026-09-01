#ifndef MODE_UPLOAD_H
#define MODE_UPLOAD_H

#include <stdbool.h>

// 上傳模式：連線 WiFi、嘗試上傳所有待傳生理資料。
// 無論成功或失敗都會返回——呼叫端（state_machine）固定轉回 BLE 接收模式。
void mode_upload_run(void);

// WiFi 連線失敗後是否還在退避冷卻期內——見 mode_upload.c 開頭
// WIFI_FAILURE_BACKOFF_MS 的說明。mode_ble_receive.c 的自動觸發條件（NTP
// 重試/待傳佇列閒置逾時）呼叫這個函式決定要不要跳過這次自動觸發，避免 WiFi
// 密碼設錯之類的情況下，裝置一直被拉回去做注定失敗、又要等很久才逾時的連線
// 嘗試，佔掉原本該用來掃描 BLE 裝置的時間。KEY1 手動觸發不受這個冷卻期限制
// （使用者主動要求，見 mode_ble_receive.c KEY1 分支的說明）。
bool mode_upload_in_backoff(void);

#endif // MODE_UPLOAD_H
