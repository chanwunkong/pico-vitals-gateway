#ifndef WALL_CLOCK_H
#define WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Pico 開機時沒有網路，沒辦法知道真實時間，量測時只能記錄 boot-relative 的
// ms（to_ms_since_boot()）。這個模組只在 WiFi 已經連上時才有用：跟公開網路
// 上的 NTP 伺服器要一次現在的真實時間，記住「校時那一刻的 boot ms 對應到哪個
// 真實世界 epoch ms」，之後任何 boot-relative 時間點都能換算成真實時間。

// 嘗試透過 SNTP 校時，只能在 WiFi 已連線、DNS 可解析時呼叫。
// **距離上次成功校時不到 NTP_RESYNC_INTERVAL_MS（見 wall_clock.c）的話直接
// 回傳 true、沿用現有基準點**，不會浪費時間再打一次 NTP，除非呼叫過
// wall_clock_request_resync()。真的要打網路時，會依序嘗試好幾個公開 NTP
// 伺服器（其中一個沒回應就換下一個），timeout_ms 是分給這些伺服器的總預算。
// 回傳是否「目前有可用的時間基準」（可能是這次新校時成功、也可能是沿用先前
// 校時過的舊基準點）；只有從來沒成功過才會回傳 false。
bool wall_clock_sync(uint32_t timeout_ms);

// 標記下一次 wall_clock_sync() 呼叫要無視距離上次成功校時多久，強制真的打
// 一次網路重新查詢。用過一次就會自動清除，不會持續強制下去。
void wall_clock_request_resync(void);

// 是否已經成功校時過（開機以來至少一次）。
bool wall_clock_is_synced(void);

// 把 boot-relative 的 ms（to_ms_since_boot() 拿到的值）換算成真實世界的
// epoch ms（自 1970-01-01 UTC 起的毫秒數）。從來沒校時成功過的話，原樣傳回
// boot-relative 值（呼叫端可以用 wall_clock_is_synced() 分辨兩種情況）。
uint64_t wall_clock_to_epoch_ms(uint64_t boot_ms);

// 血壓計自己回報的量測時間戳（device_measured_key）不保證裝置內部時鐘校時
// 過——電池換過、從沒設定過、韌體預設值都可能讓這個時鐘跟真實時間差很多年。
// 跟 Pico 自己 NTP 校時過的現在時間比對，差距超過這個範圍就不信任裝置時鐘。
// 這個值待與主持人確認，見 PROJECT_PLAN.md 第 7.3 節。上傳（mode_upload.c）
// 跟畫面顯示（display_status.c）共用同一個門檻，見下面 wall_clock_epoch_is_plausible()。
#define DEVICE_CLOCK_SANITY_WINDOW_MS (7ULL * 24 * 60 * 60 * 1000)

// 判斷某個「宣稱是真實世界 epoch ms」的時間點，跟 Pico 自己已校時過的現在時間
// 比對起來合不合理（差距是否在 window_ms 之內，通常傳 DEVICE_CLOCK_SANITY_WINDOW_MS）。
// 給有自己內部時鐘、但校時狀態不受 Pico 控制的裝置用（目前是血壓計的
// device_measured_key，見 fora_protocol_measured_key_to_epoch_ms()）——裝置的
// 時鐘可能從沒設定過、電池換過，回報出離譜的日期時間，這種情況不該被當真。
// 呼叫端必須先確認 wall_clock_is_synced() 為 true 才呼叫這個函式，否則 Pico
// 自己都沒有可信的現在時間可以比對；上傳（mode_upload.c）跟畫面顯示
// （display_status.c）共用同一份判斷邏輯，避免兩邊各自實作、之後改一邊忘記
// 改另一邊。
bool wall_clock_epoch_is_plausible(uint64_t epoch_ms, uint64_t window_ms);

#endif // WALL_CLOCK_H
