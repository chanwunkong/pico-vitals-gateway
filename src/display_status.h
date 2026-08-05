#ifndef DISPLAY_STATUS_H
#define DISPLAY_STATUS_H

#include "common.h"
#include <stddef.h>
#include <stdint.h>

// Waveshare Pico-ePaper-2.9 電子紙顯示器封裝。規劃/接線見 PROJECT_PLAN.md 第 12 節。
//
// **重要：目前只有帶 ASCII 字型（見 epd/Fonts/），沒有帶任何中文字型**（見
// PROJECT_PLAN.md 12.3 節）。這裡所有函式的字串參數（狀態文字、錯誤訊息等）
// 一律只能是英數字/半形符號。使用者自由輸入的欄位（例如 patient_id）在畫面
// 顯示前會經過 ASCII 過濾，但**呼叫端自己組的狀態文字/錯誤訊息不會被過濾**，
// 直接傳中文字串進來會顯示亂碼（也是同樣的 (char - ' ') 位址偏移量問題）。

// 初始化 SPI1/GPIO、電子紙開機、清空畫面。必須在 stdio_init_all() 之後呼叫
// （沿用同一個序列埠，不會重複初始化）。
void display_status_init(void);

// 開機驗證用畫面：固定文字＋整片全刷，用來確認接線/方向/字型顯示正確
// （2026-08-05 已實機驗證過，見 PROJECT_PLAN.md 12 節）。main.c 開機時呼叫一次。
void display_status_show_boot_test(void);

// 熱點設定模式（AP_CONFIG）畫面：熱點 SSID/密碼 + 目前已存的個案編號（沒有
// 就顯示 "(unset)"）。整個模式期間內容不會變，呼叫一次即可，不需要輪詢。
void display_status_show_ap_config(const char *ap_ssid, const char *ap_password,
                                    const device_config_t *existing_config);

// 上傳模式（UPLOAD）畫面：WiFi SSID + 目前階段/結果文字（例如 "Connecting..."、
// "Success"、"Failed, will retry"）。呼叫端自己在幾個關鍵時間點呼叫，不需要輪詢。
void display_status_show_upload(const char *ssid, const char *result_text);

// 錯誤畫面：一句英文錯誤訊息，取代難記的 LED 三連閃燈號。
void display_status_show_error(const char *message);

// BLE 接收模式（BLE_RECEIVE）目前想顯示的狀態文字（例如 "Scanning..."）。
// 只是記錄下來，不會馬上刷新畫面——實際要不要刷新由 display_status_poll()
// 決定（見該函式的說明：全刷會阻塞主迴圈，不能每次呼叫這裡就刷一次）。
//
// **狀態文字裡務必帶時間戳，不要用純靜態的 "Scanning..."**：電子紙斷電/
// 當機時畫面會凍結在最後一次刷新的內容，如果狀態文字本身不含時間資訊，
// 使用者沒辦法從畫面分辨「裝置還活著、只是沒掃到裝置」跟「裝置已經當機」。
// mode_ble_receive.c 用 display_status_format_clock() 組出帶時間戳的狀態
// 文字（例如 "Scanning (last: 14:32)"），並且會定期（不只是靠掃描到裝置才
// 觸發）更新，見該檔案的說明。
void display_status_set_ble_receive(const device_config_t *config, const char *status_text);

// 把 boot-relative ms 格式化成「HH:MM」（已校時）或「unsynced,+Nm」（未校時，
// N 是開機經過分鐘數）。跟 BLE_RECEIVE 畫面上其他讀值時間戳用的是同一份
// 格式化邏輯，開放給其他模組組裝含時間戳的狀態文字用（見上面
// display_status_set_ble_receive() 的說明）。
void display_status_format_clock(uint64_t boot_ms, char *out, size_t out_size);

// 非阻塞，只給 mode_ble_receive.c 的主迴圈每輪呼叫（跟 led_status_poll() 一樣
// 的呼叫慣例）。只有在「BLE_RECEIVE 想顯示的內容（狀態文字/各生理值最後讀值/
// 待傳筆數）真的變了」的時候才會觸發一次全刷，避免每輪迴圈都刷新（全刷約 3
// 秒、阻塞主迴圈，見 PROJECT_PLAN.md 12.5 節）。
// **注意**：這裡永遠畫的是 BLE_RECEIVE 畫面，AP_CONFIG／UPLOAD／錯誤畫面
// 不要呼叫這個函式（會被這裡覆蓋掉）——那幾個模式改呼叫對應的
// display_status_show_xxx()，內容在該模式執行期間變動不頻繁，不需要輪詢。
void display_status_poll(void);

#endif // DISPLAY_STATUS_H
