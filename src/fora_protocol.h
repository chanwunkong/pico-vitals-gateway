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

// 血糖/MD6 記錄 byte[7] 高 2 bit 的量測情境，數值跟原始 bit pattern 直接對應
// （0/1/2/3），2026-08-26 反編譯官方 PC 端程式（`BloodGlucose` class）逐行核對
// 確認：0=一般（官方程式碼還有「運動」子狀態，見 byte[3] bits5-7==4，這個
// 專案目前不解析）、1=飯前(AC)、2=飯後(PC)、3=QC（品管/對照液測試，包含
// 任何無法辨識的值，官方程式碼是 `default: QC`）。QC 不是病人數值，
// `fora_protocol_parse_reading()` 會直接濾掉、不會回傳成一筆記錄，所以
// `vital_record_t.measurement_mode`／這個 enum 不會出現代表 QC 的值。
typedef enum {
    FORA_MEASUREMENT_MODE_GEN = 0,
    FORA_MEASUREMENT_MODE_AC = 1,
    FORA_MEASUREMENT_MODE_PC = 2,
} fora_measurement_mode_t;

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
    // FORA MD6 六合一測試儀，廣播名稱含 "MD6"（2026-08-26 實機確認，見
    // PROJECT_PLAN.md 第 6.5 節）。走跟血壓計完全相同的指令管道（同樣需要
    // 配對、同樣用 FORA_BP_CMD_GET_RECORD_PART_A/B 兩段式取記錄），但六個
    // 項目的數值單位/scale 都還沒實機驗證過，目前 mode_ble_receive.c 只把
    // 收到的記錄印出來人工比對，不會存進待傳佇列/上傳，見該檔案的說明。
    FORA_DEVICE_MD6,
    // Bionime Rightest GM700SB 血糖機——**不是 FORA 裝置**，跟這個 enum 裡其他
    // 型號完全不同廠牌/晶片/協定（Dialog Semiconductor DA1458x，走自訂
    // Service 0xFEE0，不是 FORA 系列共用的 Nordic LED/Button Service pipe），
    // 真正的協定/解析邏輯在 rightest_protocol.h/.c。放進這個 enum 純粹是因為
    // `source_kind`／這個 COUNT 常數已經是專案裡「裝置種類」的統一編號空間，
    // 被 mode_ble_receive.c/mode_upload.c/storage.c/upload_api.c/
    // display_status.c 好幾個依 [FORA_DEVICE_KIND_COUNT] 大小配置的陣列、迴圈
    // 共用（見 mode_upload.c 依 source_kind 分組上傳的迴圈）——另外開一個
    // 獨立的 enum 型別會導致這些陣列/迴圈邊界對不上，風險比「名字裡有 FORA
    // 但其實不是」這個語意瑕疵更高，2026-08-28 決定沿用同一個編號空間。
    FORA_DEVICE_RIGHTEST_GM700SB,
    FORA_DEVICE_KIND_COUNT,
} fora_device_kind_t;

// 依掃描到的廣播封包判斷是否為要連線的 FORA 裝置：比對裝置名稱是否包含
// "FORA"，並依名稱進一步判斷是哪一種型號（*out_kind，可傳 NULL 不取）——
// 用名稱有沒有包含 "O2" 分辨血氧計、"D40" 分辨血壓計、"IR42" 分辨額溫槍、
// "MD6" 分辨六合一測試儀（全部 2026-08-26 實機用序列埠監控確認過，見
// PROJECT_PLAN.md 第 6 節）。**每一種都要明確比對到關鍵字才算數**：名稱含
// "FORA" 但比對不出任何已知型號會回傳 false，不會像舊版那樣預設當成額溫
// 槍——曾經真的發生過 MD6 被誤判成額溫槍、回應被誤解成一筆體溫讀值上傳
// 出去的事故，不能讓「認不出型號」的裝置有機會被當成任何一種已支援的裝置
// 去連線/解析。
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
//   MD6 kind（1 筆，HCT 是 2 筆）：跟血壓計一樣是呼叫端已經組好的 8 bytes
//     記錄，日期時間解碼方式相同，但 byte[7] 換成「量測情境（bits6-7）＋
//     項目代碼（bits2-5）」——QC（品管/對照液測試，情境代碼 3）只在血糖項目
//     視為無效讀值不回傳，其餘 5 項不套用這個過濾（見 fora_protocol.c MD6
//     分支的說明）；項目代碼對應 VITAL_TYPE_GLUCOSE/HCT/KETONE/UA/CHOL/HB
//     其中一種，認不出的代碼（MD6 六合一不會用到的乳酸/三酸甘油脂）也不
//     回傳。**HCT 額外多回傳第 2 筆換算出來的 VITAL_TYPE_HB**（裝置沒有
//     獨立的 Hb 記錄可以要，見 fora_protocol.c 的說明，這筆是估計值不是
//     裝置量到的）。
size_t fora_protocol_parse_reading(
    fora_device_kind_t kind, const uint8_t *value, uint16_t len,
    vital_record_t out[FORA_MAX_READINGS_PER_NOTIFICATION]);

#endif // FORA_PROTOCOL_H
