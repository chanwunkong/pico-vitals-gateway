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

// KEY1 邊緣觸發：偵測到「這一次輪詢按下、上一次輪詢沒按下」才回傳 true，
// 放開再重新按一次才會再觸發一次。
bool button_input_key1_pressed(void);

// KEY2 邊緣觸發，用法同 button_input_key1_pressed()。
bool button_input_key2_pressed(void);

#endif // BUTTON_INPUT_H
