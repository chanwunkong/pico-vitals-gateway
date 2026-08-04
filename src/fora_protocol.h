#ifndef FORA_PROTOCOL_H
#define FORA_PROTOCOL_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// FORA IR42 額溫槍實際協定（2026-07-31 靠舊版已驗證可動的 MicroPython 實作確認，
// 不是標準 Bluetooth SIG Health Thermometer Service）：
//   裝置雖然也宣告 Health Thermometer (0x1809) / Device Information (0x180A)，
//   但實際資料是透過 Nordic nRF5 SDK 範例板內建的自訂
//   "LED and Button Service" characteristic 傳輸（FORA/Taidoc 常見 OEM 做法，
//   借用 Nordic SDK 現成的雙向 pipe characteristic 來傳自己的封包格式）：
//     Service:        00001523-1212-efde-1523-785feabcd123
//     Characteristic: 00001524-1212-efde-1523-785feabcd123（屬性 Write + Notify）
//   訂閱 Notify 之後，還要主動寫入一段固定的觸發指令，裝置才會把目前量到的
//   數值回傳（不會自動推播）。收到的回應第一個 byte 是 0x51，
//   溫度值（攝氏 x10）是小端 16-bit，取 byte[2]/byte[3] 組成後遮罩低 12 bit。

#define FORA_SERVICE_UUID128_INIT { \
    0x00, 0x00, 0x15, 0x23, 0x12, 0x12, 0xEF, 0xDE, \
    0x15, 0x23, 0x78, 0x5F, 0xEA, 0xBC, 0xD1, 0x23 }
#define FORA_CHARACTERISTIC_UUID128_INIT { \
    0x00, 0x00, 0x15, 0x24, 0x12, 0x12, 0xEF, 0xDE, \
    0x15, 0x23, 0x78, 0x5F, 0xEA, 0xBC, 0xD1, 0x23 }

extern const uint8_t FORA_SERVICE_UUID128[16];
extern const uint8_t FORA_CHARACTERISTIC_UUID128[16];

// 訂閱 Notify 成功後要送出的觸發指令，裝置才會回傳目前量到的數值。
extern const uint8_t FORA_TRIGGER_COMMAND[8];

// FORA D40（血壓血糖計，血壓部分）用的是標準 Bluetooth SIG "Blood Pressure
// Service"，不是上面那套自訂 pipe，服務/特徵值都是官方 16-bit UUID，特徵值
// 屬性是 Indicate（不是 Notify），裝置自己量完血壓會主動推播，不需要、也不能
// 送 FORA_TRIGGER_COMMAND 給它。
#define BLOOD_PRESSURE_SERVICE_UUID16      0x1810u
#define BLOOD_PRESSURE_MEASUREMENT_UUID16  0x2A35u

// 手上目前有的三種 FORA OEM 裝置。額溫槍跟血氧計服務/特徵值 UUID 完全相同
// （同一套 Nordic LED/Button Service 傳輸管道），只有封包格式不同；血壓計走
// 標準 Blood Pressure Service（見上）。UNKNOWN 用來代表「名稱含 FORA，但不
// 認得是哪一種」，呼叫端應該當作陌生裝置處理（不要連線）。
typedef enum {
    FORA_DEVICE_UNKNOWN = 0,
    FORA_DEVICE_THERMOMETER,
    FORA_DEVICE_OXIMETER,
    FORA_DEVICE_BLOOD_PRESSURE,
    FORA_DEVICE_KIND_COUNT,
} fora_device_kind_t;

// 依掃描到的廣播封包判斷是否為要連線的 FORA 裝置：比對裝置名稱是否包含 "FORA"，
// 並依名稱進一步判斷是哪一種型號（*out_kind，可傳 NULL 不取）——目前用名稱
// 有沒有包含 "O2" 分辨血氧計、"D40" 分辨血壓計，其餘視為額溫槍。
bool fora_protocol_matches_advertisement(
    const uint8_t *adv_data, uint8_t adv_len, fora_device_kind_t *out_kind);

// 血壓計一次量測最多會同時帶收縮壓、舒張壓、脈搏三筆數值——用陣列讓呼叫端
// 一次拿完，不用呼叫多次。
#define FORA_MAX_READINGS_PER_NOTIFICATION 3

// 解析收到的 notify/indicate payload，依 kind 決定用哪一種格式解析，成功時
// 把讀值填進 out[]，回傳實際填入的筆數（0 表示格式不符，out 內容不可用）：
//   額溫槍（1 筆）：byte[0]==0x51 才是有效回應，
//     溫度 = ((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0，單位攝氏。
//   血氧計（2 筆，2026-08-03 靠實際量測比對回推確認）：byte[0]==0x51 才是
//     有效回應，SpO2 = (byte[3]<<8 | byte[2]) & 0x0FFF（單位 %，不用除 10），
//     脈搏 = byte[5]（單位 bpm，整數）。byte[4]/byte[6]/byte[7] 目前不知道
//     用途，先忽略。
//   血壓計（2~3 筆，標準 Blood Pressure Measurement 0x2A35 格式，非自訂）：
//     byte[0] 是 flags；byte[1..2]/byte[3..4] 是收縮壓/舒張壓（皆為
//     IEEE-11073 SFLOAT，小端）；若 flags bit2 有設，代表後面（跳過 time
//     stamp 欄位，若 flags bit1 也有設）還帶一筆脈搏（同樣是 SFLOAT）。目前
//     假設裝置回報單位是 mmHg（沒特別處理 flags bit0 的 kPa 情況)。
size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]);

#endif // FORA_PROTOCOL_H
