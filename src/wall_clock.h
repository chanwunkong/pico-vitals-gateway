#ifndef WALL_CLOCK_H
#define WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Pico 開機時沒有網路，沒辦法知道真實時間，量測時只能記錄 boot-relative 的
// ms（to_ms_since_boot()）。這個模組只在 WiFi 已經連上時才有用：跟公開網路
// 上的 NTP 伺服器要一次現在的真實時間，記住「校時那一刻的 boot ms 對應到哪個
// 真實世界 epoch ms」，之後任何 boot-relative 時間點都能換算成真實時間。

// 嘗試透過 SNTP 校時，只能在 WiFi 已連線、DNS 可解析時呼叫。
// **每天只會真的打一次網路**：距離上次成功校時不到 24 小時的話直接回傳
// true、沿用現有基準點，不會浪費時間再打一次 NTP（RP2040 的震盪器漂移量級
// 是一天幾秒，不需要每次上傳都重新校時）。真的要打網路時，會依序嘗試好幾個
// 公開 NTP 伺服器（其中一個沒回應就換下一個），timeout_ms 是分給這些伺服器
// 的總預算。回傳是否「目前有可用的時間基準」（可能是這次新校時成功、也可能
// 是沿用先前校時過的舊基準點）；只有從來沒成功過才會回傳 false。
bool wall_clock_sync(uint32_t timeout_ms);

// 是否已經成功校時過（開機以來至少一次）。
bool wall_clock_is_synced(void);

// 把 boot-relative 的 ms（to_ms_since_boot() 拿到的值）換算成真實世界的
// epoch ms（自 1970-01-01 UTC 起的毫秒數）。從來沒校時成功過的話，原樣傳回
// boot-relative 值（呼叫端可以用 wall_clock_is_synced() 分辨兩種情況）。
uint64_t wall_clock_to_epoch_ms(uint64_t boot_ms);

#endif // WALL_CLOCK_H
