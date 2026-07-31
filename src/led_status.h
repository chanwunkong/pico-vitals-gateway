#ifndef LED_STATUS_H
#define LED_STATUS_H

// LED 燈號規範見 PROJECT_PLAN.md 第 4 節。
typedef enum {
    LED_OFF,
    LED_SOLID_ON,       // 熱點設定模式
    LED_FAST_BLINK,     // 開機 BOOTSEL 偵測視窗 (100ms on/off)
    LED_SLOW_BLINK,     // BLE 掃描/重連中 (1000ms on/off)
    LED_HEARTBEAT,      // BLE 已連線、正常接收 (每2000ms閃50ms)
    LED_UPLOAD_BLINK,   // 上傳模式 (150ms on/off)
    LED_ERROR_BURST,    // 錯誤/異常 (三連閃+停頓)
} led_pattern_t;

// 必須在 cyw43_arch_init() 成功之後才能呼叫（LED 是接在 CYW43 晶片上，
// 不是一般 RP2040 GPIO）。
void led_status_init(void);

void led_status_set(led_pattern_t pattern);

// 非阻塞，需在主協作式迴圈中頻繁呼叫（例如每次迴圈或每 20ms），
// 不可放進中斷處理常式（曾有回報在計時器 ISR 中呼叫、網路忙碌時會 hang）。
void led_status_poll(void);

#endif // LED_STATUS_H
