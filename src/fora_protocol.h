#ifndef FORA_PROTOCOL_H
#define FORA_PROTOCOL_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// FORA IR42 額溫槍協定：裝置雖然宣告標準 Health Thermometer (0x1809)，但
// 實際資料透過自訂的 Nordic LED/Button Service characteristic 傳輸：
//   Service:        00001523-1212-efde-1523-785feabcd123
//   Characteristic: 00001524-1212-efde-1523-785feabcd123（Write + Notify）
// 訂閱 Notify 後需主動寫入觸發指令，裝置才會回傳目前量到的數值（不會自動
// 推播）。回應第一個 byte 是 0x51，溫度值（攝氏 x10）是小端 16-bit，
// 取 byte[2]/byte[3] 組成後遮罩低 12 bit。

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

// FORA D40 血壓計（跟血糖共用同一個 kind，見下方 fora_device_kind_t）走跟
// 額溫槍/血氧計相同的自訂 pipe，用「寫入指令、等 Notify 回應」取得目前的
// 量測記錄。指令格式統一是 8 bytes：
//   { 0x51, cmd, p1, p2, p3, p4, 0xA3, checksum }
// checksum = 前 7 bytes 總和的低位元組。
//
// 取得「目前這一筆」記錄的流程（p4=使用者編號，固定用 0；p1/p2/p3 固定填 0）：
//   1. 送 { 0x51, 0x25, 0, 0, 0, 0, 0xA3, checksum } → 回應取 byte[2..5]
//   2. 送 { 0x51, 0x26, 0, 0, 0, 0, 0xA3, checksum } → 回應取 byte[2..5]
//   3. 把兩次收到的各 4 bytes 接起來，變成 8 bytes，交給
//      fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...) 解析。
//
// 8 bytes 佈局：
//   byte[0] = 日期低位元組（day bits0-4、month 低3 bit 在 bits5-7）
//   byte[1] = 日期高位元組（month 最高 1 bit、year=(byte1>>1)+2000）
//   byte[2] = 分鐘(bits0-5)、心律不整旗標(bit6)、記錄類型旗標(bit7：
//             0=血糖、1=血壓)
//   byte[3] = 小時(bits0-4)、IHB 狀態(bits5-6)、是否為平均值(bit7)
//   血壓（byte[2] bit7 == 1）：
//     byte[4] = 收縮壓（整數 mmHg）
//     byte[5] = 平均壓（不取）
//     byte[6] = 舒張壓（整數 mmHg）
//     byte[7] = 脈搏（整數 bpm）
//   血糖（byte[2] bit7 == 0）：
//     byte[4..5] = 血糖值，16-bit 小端：glucose = byte[5]*256 + byte[4]，
//                  單位 mg/dL；65535 或 255 視為無效讀值
//     byte[6]    = 環境溫度（不取）
//     byte[7]    = codeNo(bits0-5，不取) | 測試時機(bits6-7，不取)
// 日期/心律不整/IHB 狀態目前沒有解析（vital_record_t 沒有對應欄位存）。
//
// 想問裝置目前有幾筆記錄（目前用不到）：
//   送 { 0x51, 0x2B, userNo, 0, 0, 0, 0xA3, checksum } → 回應
//   byte[2] | (byte[3]<<8) = 筆數
#define FORA_BP_CMD_GET_RECORD_PART_A 0x25
#define FORA_BP_CMD_GET_RECORD_PART_B 0x26
#define FORA_BP_CMD_GET_RECORD_COUNT  0x2B
#define FORA_BP_USER_CURRENT          0x00

// 組出上面說明的 8 byte 指令（含自動算好的 checksum，見 out[7]）。
void fora_protocol_build_command(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t out[8]);

// 把血壓/血糖記錄 8 bytes 裡的日期/時間欄位（byte[0..3]）解碼成一個分鐘
// 解析度、可以直接比較是否相等的唯一值，給 storage.c 判斷「這筆記錄跟上一筆
// 是不是同一次量測」用，不是真正的 epoch time（見 common.h
// vital_record_t.device_measured_key 欄位的說明）。0 保留給「這種裝置協定
// 沒有量測時間戳可用」（額溫槍/血氧計），血壓/血糖路徑不會回傳 0。
uint32_t fora_protocol_decode_measured_key(const uint8_t record[8]);

// 把上面那個鍵值還原回年/月/日/時/分（分鐘解析度，沒有秒）。
void fora_protocol_measured_key_to_datetime(
    uint32_t key, unsigned *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute);

// 把量測時間戳鍵值換算成 epoch ms（UTC），假設裝置內部時鐘存的是本地時間
// （見 common.h 的 LOCAL_UTC_OFFSET_SEC）。
uint64_t fora_protocol_measured_key_to_epoch_ms(uint32_t key);

// 手上目前有的三種 FORA OEM 裝置，都走同一套 Nordic LED/Button Service
// 自訂 pipe，只有封包格式不同；血壓計這個 kind 同時涵蓋血壓跟血糖兩種資料
// （見上方協定說明），血糖不是獨立的 kind，因為裝置廣播/連線階段無法分辨，
// 要連線問到記錄之後才知道類型。UNKNOWN 代表「名稱含 FORA，但不認得是哪一
// 種」，呼叫端應該當作陌生裝置處理（不要連線）。
typedef enum {
    FORA_DEVICE_UNKNOWN = 0,
    FORA_DEVICE_THERMOMETER,
    FORA_DEVICE_OXIMETER,
    FORA_DEVICE_BLOOD_PRESSURE,
    FORA_DEVICE_KIND_COUNT,
} fora_device_kind_t;

// 依掃描到的廣播封包判斷是否為要連線的 FORA 裝置：比對裝置名稱是否包含
// "FORA"，並依名稱進一步判斷是哪一種型號（*out_kind，可傳 NULL 不取）——
// 用名稱有沒有包含 "O2" 分辨血氧計、"D40" 分辨血壓計，其餘視為額溫槍。
bool fora_protocol_matches_advertisement(
    const uint8_t *adv_data, uint8_t adv_len, fora_device_kind_t *out_kind);

// 血壓計一次量測最多會同時帶收縮壓、舒張壓、脈搏三筆數值。
#define FORA_MAX_READINGS_PER_NOTIFICATION 3

// 解析收到的資料，依 kind 決定用哪一種格式解析，成功時把讀值填進 out[]，
// 回傳實際填入的筆數（0 表示格式不符，out 內容不可用）：
//   額溫槍（1 筆）：byte[0]==0x51 才是有效回應，
//     溫度 = ((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0，單位攝氏。
//   血氧計（2 筆）：byte[0]==0x51 才是有效回應，
//     SpO2 = (byte[3]<<8 | byte[2]) & 0x0FFF（單位 %，不除 10），
//     脈搏 = byte[5]（單位 bpm，整數）。byte[4]/byte[6]/byte[7] 不用。
//   血壓計 kind（3 筆血壓，或 1 筆血糖）：value/len 是呼叫端已經組好的
//     8 bytes 記錄（見上方 FORA_BP_CMD_* 說明），不檢查 byte[0]==0x51。
//     依上方協定說明的 byte[2] bit7 分辨血壓/血糖並各自解析。
size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]);

#endif // FORA_PROTOCOL_H
