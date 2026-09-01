#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H

#include <stdbool.h>
#include <stdint.h>

// Waveshare Pico-CapTouch-ePaper-2.9 板載按鍵 KEY0/KEY1/KEY2（GP2/GP3/GP15，
// 內建上拉，按下時為低電位）。沒有接這片電子紙的機器（純 Pico W，見
// PROJECT_PLAN.md）讀到的一律是「沒按下」，不影響運作，不需要另外用編譯選項
// 區分兩種硬體。
void button_input_init(void);

// 非阻塞輪詢，要在主迴圈裡持續呼叫。KEY0 連續按住達 hold_ms 毫秒才回傳一次
// true，放開後才會重新開始計時，同一次按住不會重複觸發。呼叫端可以用不同的
// hold_ms 在不同模式的迴圈裡各自呼叫（例如進入 AP_CONFIG 用 3 秒、在
// AP_CONFIG 裡取消用 1 秒），追蹤的是同一個實體按鍵的連續按住時間，不會互相干擾。
bool button_input_key0_long_press(uint32_t hold_ms);

// KEY1 短按：跟下面的 button_input_key1_long_press() 共用同一顆實體按鍵，
// 用按住時間長短分流成兩種動作（2026-09-01 起，之前只有短按一種手勢）。
// 在「放開的那一刻」才判定，且只有按住時間小於 long_press_threshold_ms
// 才算數——按住超過門檻的話，那次放開不會觸發短按（因為已經被長按那邊在
// 按住期間搶先觸發過了），呼叫端要傳跟 button_input_key1_long_press() 同一個
// hold_ms，兩邊門檻才會一致。
bool button_input_key1_pressed(uint32_t long_press_threshold_ms);

// KEY1 長按：用法同 button_input_key0_long_press()，按住達 hold_ms 才回傳一次
// true，放開後重新計時。跟上面 button_input_key1_pressed() 各自獨立追蹤按下/
// 放開時間，不共用內部狀態，但呼叫端要傳相同的 hold_ms 值，語意才會一致
// （短按＝放開時按住時間 < hold_ms，長按＝按住時間達到 hold_ms）。
bool button_input_key1_long_press(uint32_t hold_ms);

// KEY2 邊緣觸發，用法同 button_input_key1_pressed() 的短按版本（沒有長按）。
bool button_input_key2_pressed(void);

// KEY2 這一刻是不是正被按住——跟上面的邊緣觸發版本是分開的即時讀值，開機時
// main.c 檢查一次用（「開機時按住 KEY2」＝清除 flash 裡的待傳佇列/設定，見
// storage_factory_reset() 的說明），不會互相干擾彼此的內部狀態。
bool button_input_key2_is_held(void);

#endif // BUTTON_INPUT_H
