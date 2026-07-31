#ifndef FORA_PROTOCOL_H
#define FORA_PROTOCOL_H

#include "common.h"
#include <stdbool.h>
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

// 依掃描到的廣播封包判斷是否為要連線的 FORA 裝置：比對裝置名稱是否包含 "FORA"。
bool fora_protocol_matches_advertisement(const uint8_t *adv_data, uint8_t adv_len);

// 解析觸發指令後收到的 notify payload：byte[0]==0x51 才是有效回應，
// 溫度 = ((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0，單位攝氏。
// 成功時填入 out 並回傳 true。
bool fora_protocol_parse_reading(const uint8_t *value, uint16_t len, vital_record_t *out);

#endif // FORA_PROTOCOL_H
