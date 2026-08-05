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

// 2026-08-05 更正：FORA D40 血壓計**不是**走標準 Bluetooth SIG Blood Pressure
// Service——之前（見 PROJECT_PLAN.md 第 6.3 節）一度以為連線後直接 Read
// `0x2A35` 能拿到值，後來實測連線列出這個服務底下的特徵值，發現
// `properties` 只有 0x20（純 Indicate，沒有 Read），Read 每次都被拒絕
// （att_status=0x02 Read Not Permitted），改成訂閱 Indicate 耐心等，等了 76
// 秒依然等不到裝置主動推播——這台裝置量測完才開始廣播，連線建立時量測早就
// 結束了，Indicate 只會推播「訂閱之後才發生」的新事件，不會補送舊結果。
//
// 真正的做法（2026-08-05 反編譯官方 Windows 程式 TaiDoc.BLE_PcLink 才確認）：
// **這台裝置走跟額溫槍/血氧計完全相同的 Nordic LED/Button Service 自訂
// pipe**（上面的 FORA_SERVICE_UUID128／FORA_CHARACTERISTIC_UUID128），用
// 「寫入指令、等 Notify 回應」的方式，一問一答地跟裝置要目前的量測記錄，
// 不是被動等推播。指令格式統一是 8 bytes：
//   { 0x51, cmd, p1, p2, p3, p4, 0xA3, checksum }
// checksum = 前 7 bytes 總和的低位元組（跟 FORA_TRIGGER_COMMAND 最後一個
// byte 用同一套算法，可交叉驗證：0x51+0x26+0xA3 之後取低位元組 = 0x1A，
// 剛好等於 FORA_TRIGGER_COMMAND 的最後一個 byte）。
//
// 取得「目前這一筆」血壓記錄的流程（p1=記錄索引低位元組、p2=索引高位元組、
// p4=使用者編號，這台裝置固定用 0（CurrentUser）；p3 目前固定填 0）：
//   1. 送 { 0x51, 0x25, 0, 0, 0, 0, 0xA3, checksum }（索引 0 = 最新一筆）
//      → 收到的回應取 byte[2..5]（4 bytes）
//   2. 送 { 0x51, 0x26, 0, 0, 0, 0, 0xA3, checksum }（同一筆記錄的另外一半）
//      → 收到的回應取 byte[2..5]（4 bytes）
//   3. 把兩次收到的各 4 bytes 接起來，變成 8 bytes，交給
//      fora_protocol_parse_reading(FORA_DEVICE_BLOOD_PRESSURE, ...) 解析
//      （這裡傳進去的是「組好的 8 bytes」，不是原始 GATT 封包）。
// 8 bytes 的實際佈局（跟標準 SIG 格式完全不同，是這台裝置自己的私有格式）：
//   byte[0] = 日期低位元組（day bits0-4、month 低3 bit 在 bits5-7）
//   byte[1] = 日期高位元組（month 最高 1 bit、year=(byte1>>1)+2000）
//   byte[2] = 分鐘(bits0-5)、心律不整旗標(bit6)
//   byte[3] = 小時(bits0-4)、IHB 狀態(bits5-6)、是否為平均值(bit7)
//   byte[4] = 收縮壓（直接是整數 mmHg，不是 SFLOAT）
//   byte[5] = 平均壓（目前不取）
//   byte[6] = 舒張壓（直接是整數 mmHg）
//   byte[7] = 脈搏（直接是整數 bpm）
// 日期/心律不整/IHB 狀態目前沒有解析（vital_record_t 沒有對應欄位存），只取
// 收縮壓/舒張壓/脈搏三個數值。
//
// 想問裝置目前有幾筆記錄（目前用不到，但一起記錄協定）：
//   送 { 0x51, 0x2B, userNo, 0, 0, 0, 0xA3, checksum } → 回應 byte[2] | (byte[3]<<8) = 筆數
#define FORA_BP_CMD_GET_RECORD_PART_A 0x25
#define FORA_BP_CMD_GET_RECORD_PART_B 0x26
#define FORA_BP_CMD_GET_RECORD_COUNT  0x2B
#define FORA_BP_USER_CURRENT          0x00

// 組出上面說明的 8 byte 指令（含自動算好的 checksum，見 out[7]）。
void fora_protocol_build_command(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t out[8]);

// 把上面說明的血壓記錄 8 bytes 裡的日期/時間欄位（byte[0..3]）解碼成一個
// 分鐘解析度、可以直接比較是否相等的唯一值，給 storage.c 判斷「這筆記錄跟
// 上一筆是不是同一次量測」用，不是真正的 epoch time（見 common.h
// vital_record_t.device_measured_key 欄位的說明）。0 保留給「這種裝置協定
// 沒有量測時間戳可用」（額溫槍/血氧計），呼叫這個函式的血壓計路徑不會回傳
// 0（day 欄位至少是 1，day<<11 恆不為 0）。
uint32_t fora_protocol_decode_measured_key(const uint8_t record[8]);

// 把上面那個鍵值還原回年/月/日/時/分（分鐘解析度，沒有秒），給需要「顯示裝置
// 自己回報的量測時間」的地方用（例如 display_status.c 畫血壓那一行，應該顯示
// 裝置量測當下的時間，不是 Pico 收到 BLE 通知那一刻的時間）。
void fora_protocol_measured_key_to_datetime(
    uint32_t key, unsigned *year, unsigned *month, unsigned *day, unsigned *hour, unsigned *minute);

// 把量測時間戳鍵值換算成 epoch ms（UTC），假設裝置內部時鐘存的是本地時間
// （見 common.h 的 LOCAL_UTC_OFFSET_SEC）——2026-08-05 實測比對過：解碼出來的
// 時分（18:39）跟量測當下的實際本地時間吻合，這個假設目前看起來成立。給
// mode_upload.c 上傳資料時用，取代「Pico 收到 BLE 通知的時間」，更準確反映
// 「裝置實際量測的時間」（裝置量完到被 Pico 連上讀到資料之間可能有延遲）。
uint64_t fora_protocol_measured_key_to_epoch_ms(uint32_t key);

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

// 解析收到的資料，依 kind 決定用哪一種格式解析，成功時把讀值填進 out[]，
// 回傳實際填入的筆數（0 表示格式不符，out 內容不可用）：
//   額溫槍（1 筆）：byte[0]==0x51 才是有效回應，
//     溫度 = ((byte[3]<<8 | byte[2]) & 0x0FFF) / 10.0，單位攝氏。
//   血氧計（2 筆，2026-08-03 靠實際量測比對回推確認）：byte[0]==0x51 才是
//     有效回應，SpO2 = (byte[3]<<8 | byte[2]) & 0x0FFF（單位 %，不用除 10），
//     脈搏 = byte[5]（單位 bpm，整數）。byte[4]/byte[6]/byte[7] 目前不知道
//     用途，先忽略。
//   血壓計（3 筆：收縮壓/舒張壓/脈搏）：這裡的 value/len 不是原始 GATT
//     封包，是呼叫端已經組好的 8 bytes「記錄」（見上面 FORA_BP_CMD_* 那段
//     說明），直接照那邊列的 byte[4]/byte[6]/byte[7] 佈局取值，不會檢查
//     byte[0]==0x51（那個檢查只適用於額溫槍/血氧計的原始封包）。
size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]);

#endif // FORA_PROTOCOL_H
