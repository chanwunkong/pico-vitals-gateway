#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H

#include <stdbool.h>
#include <stdint.h>

// Waveshare Pico-CapTouch-ePaper-2.9 板載按鍵 KEY0/KEY1/KEY2（GP2/GP3/GP15，
// 內建上拉，按下時為低電位）。沒有接這片電子紙的機器（純 Pico W，見
// PROJECT_PLAN.md）讀到的一律是「沒按下」，不影響運作，不需要另外用編譯選項
// 區分兩種硬體。
void button_input_init(void);

// 2026-09-02 起，三顆按鍵統一同一套「短按/長按」規則：按住 < hold_ms 放開
// 才算短按（放開的那一刻判定），按住 >= hold_ms 在按住途中就算長按（不用
// 等放開）。所有呼叫端目前都傳 3000（見各自 .c 檔的 *_HOLD_MS 常數），三顆
// 鍵、每個動作的分界都一致，不會有的鍵是 1 秒、有的鍵是 3 秒。

// 非阻塞輪詢，要在主迴圈裡持續呼叫。KEY0 連續按住達 hold_ms 毫秒才回傳一次
// true（按住途中觸發，不用等放開），放開後才會重新開始計時，同一次按住不會
// 重複觸發。跟下面的 button_input_key0_pressed() 共用同一組內部狀態——見
// button_input_key0_pressed() 的說明，這是刻意的，用來避免長按剛觸發、手指
// 還沒放開時，被另一個模式的迴圈接手輪詢後誤判成一次新的短按。
bool button_input_key0_long_press(uint32_t hold_ms);

// KEY0 短按：用在 AP_CONFIG 模式裡取消設定（見 mode_ap_config.c）。在「放開
// 的那一刻」才判定，且只有按住時間小於 long_press_threshold_ms 才算數。跟
// 上面 button_input_key0_long_press() 共用同一組按下/放開時間追蹤狀態（不是
// 像 KEY1 那樣各自獨立）——因為這兩個動作分別在不同模式的迴圈裡呼叫，不會
// 同時執行，共用狀態才能讓「長按進入 AP_CONFIG 時手指還沒放開」這種跨模式
// 切換的按住正確被視為同一次，不會在放開時誤觸發取消。呼叫端要傳跟
// button_input_key0_long_press() 同一個 hold_ms，兩邊門檻才會一致。
bool button_input_key0_pressed(uint32_t long_press_threshold_ms);

// KEY1 短按：跟下面的 button_input_key1_long_press() 共用同一顆實體按鍵，
// 用按住時間長短分流成兩種動作（2026-09-01 起，之前只有短按一種手勢）。
// 在「放開的那一刻」才判定，且只有按住時間小於 long_press_threshold_ms
// 才算數——按住超過門檻的話，那次放開不會觸發短按（因為已經被長按那邊在
// 按住期間搶先觸發過了），呼叫端要傳跟 button_input_key1_long_press() 同一個
// hold_ms，兩邊門檻才會一致。跟 KEY0 不同，這兩個函式各自獨立追蹤按下/放開
// 時間，不共用內部狀態——因為呼叫端在同一個迴圈裡「每一輪都同時呼叫」這兩個
// 函式（不是分屬不同模式的迴圈），各自獨立算出來的按下時間點自然一致，不需要
// 共用狀態。
bool button_input_key1_pressed(uint32_t long_press_threshold_ms);

// KEY1 長按：用法同 button_input_key0_long_press()，按住達 hold_ms 才回傳一次
// true，放開後重新計時。跟上面 button_input_key1_pressed() 各自獨立追蹤按下/
// 放開時間，不共用內部狀態，但呼叫端要傳相同的 hold_ms 值，語意才會一致
// （短按＝放開時按住時間 < hold_ms，長按＝按住時間達到 hold_ms）。
bool button_input_key1_long_press(uint32_t hold_ms);

// KEY2 短按：用法、判定時機都跟 button_input_key1_pressed() 一樣（放開時
// 判定，按住時間 < long_press_threshold_ms 才算數）。目前執行期間沒有對應
// 的 KEY2 長按動作（開機時「持續按住 KEY2 + 重新通電」的出廠重置是獨立的
// 開機檢查，見 button_input_key2_is_held()，不是這個函式的長按版本），但
// 門檻仍統一傳 3000，避免按住很久放開時被誤判成短按觸發。
bool button_input_key2_pressed(uint32_t long_press_threshold_ms);

// KEY2 這一刻是不是正被按住——跟上面的邊緣觸發版本是分開的即時讀值，開機時
// main.c 檢查一次用（「開機時按住 KEY2」＝清除 flash 裡的待傳佇列/設定，見
// storage_factory_reset() 的說明），不會互相干擾彼此的內部狀態。
bool button_input_key2_is_held(void);

#endif // BUTTON_INPUT_H
