#ifndef MODE_BOOT_SELECT_H
#define MODE_BOOT_SELECT_H

#include <stdbool.h>
#include <stdint.h>

// 在開機後的短視窗（window_ms 毫秒）內輪詢 BOOTSEL 按鈕，若偵測到按住則回傳 true
// （呼叫端應進入熱點設定模式），視窗結束都沒偵測到則回傳 false。
//
// 呼叫時機限制：必須在 BLE/WiFi 實際開始運作之前呼叫，也就是
// hci_power_control(HCI_POWER_ON)、cyw43_arch_enable_ap_mode()/enable_sta_mode()
// 都還沒呼叫過、core1 也還沒啟動的狀態下呼叫。cyw43_arch_init() 本身可以先呼叫
// （LED 顯示需要它），因為它是同步一次性初始化，不會造成持續性背景 flash 存取。
// 過了開機視窗之後，正常 24/7 運作期間絕對不要再呼叫 BOOTSEL 讀取——讀取過程需要
// 暫停 flash XIP 並關閉中斷，若和 BLE/WiFi 中斷同時發生有當機風險。
bool mode_boot_select_check(uint32_t window_ms);

#endif // MODE_BOOT_SELECT_H
