#ifndef MODE_AP_CONFIG_H
#define MODE_AP_CONFIG_H

#include <stdint.h>

// 熱點設定模式：開啟 WiFi 熱點、提供簡易網頁表單讓手機設定 WiFi 帳密與個案資訊。
// 阻塞直到收到表單送出、KEY0 取消、或（timeout_ms > 0 時）逾時才返回。
// timeout_ms == 0：不設逾時，等到表單送出或 KEY0 取消為止（KEY0 長按手動
// 重新進入這個模式時用這個）。
// timeout_ms > 0：額外加一個時間上限，逾時視同取消（不會呼叫
// storage_save_config()），見 state_machine.c 的 AP_CONFIG_BOOT_TIMEOUT_MS。
void mode_ap_config_run(uint32_t timeout_ms);

#endif // MODE_AP_CONFIG_H
