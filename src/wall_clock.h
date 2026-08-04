#ifndef WALL_CLOCK_H
#define WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Pico 開機時沒有網路，沒辦法知道真實時間，量測時只能記錄 boot-relative 的
// ms（to_ms_since_boot()）。這個模組只在 WiFi 已經連上時才有用：跟公開網路
// 上的 NTP 伺服器要一次現在的真實時間，記住「校時那一刻的 boot ms 對應到哪個
// 真實世界 epoch ms」，之後任何 boot-relative 時間點都能換算成真實時間。

// 嘗試透過 SNTP 校時，最多等 timeout_ms；只能在 WiFi 已連線、DNS 可解析時呼叫。
// 回傳是否成功拿到時間；失敗不影響上傳本身，只是這次沒辦法換算成真實時間。
bool wall_clock_sync(uint32_t timeout_ms);

// 是否已經成功校時過（開機以來至少一次）。
bool wall_clock_is_synced(void);

// 把 boot-relative 的 ms（to_ms_since_boot() 拿到的值）換算成真實世界的
// epoch ms（自 1970-01-01 UTC 起的毫秒數）。從來沒校時成功過的話，原樣傳回
// boot-relative 值（呼叫端可以用 wall_clock_is_synced() 分辨兩種情況）。
uint64_t wall_clock_to_epoch_ms(uint64_t boot_ms);

#endif // WALL_CLOCK_H
