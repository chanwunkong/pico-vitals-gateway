#ifndef RIGHTEST_PROTOCOL_H
#define RIGHTEST_PROTOCOL_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bionime Rightest GM700SB 血糖機協定，來源：廠商提供的《GM700SB Data
// Communication Protocol - Rev 1.1》PDF（2026-08-28 拿到，放在專案根目錄，
// 檔案標記 Confidential，故意不加入版控——比照這個專案一直以來對第三方
// 私有協定資料的處理方式，見 PROJECT_PLAN.md 第 6.5 節反編譯 FORA 官方 DLL
// 的說明，兩者都是「參考來源不進 repo，但協定本身寫成程式碼」）。
//
// 跟 fora_protocol.h 那幾款裝置是完全不同的廠牌/晶片/協定：GM700SB 用
// Dialog Semiconductor DA1458x BLE SoC（LightBlue 掃描 Device Information
// 服務讀到 Manufacturer="Dialog Semi"、Model="DA1458x"，2026-08-28 實機
// 確認），走自訂 GATT service，不是標準 Bluetooth SIG Glucose Profile
// （沒有 0x1808/0x2A52 RACP），也不是 FORA 系列共用的 Nordic LED/Button
// Service pipe。
//
// Service 0xFEE0 底下有 5 個 characteristic，協定文件只定義其中 3 個
// （另外掃到的 FEE5/FEE6 文件標記 Reserve，不用管，大概是韌體 profile
// 模板留下的）：
//   0xFEE1（Read/Write，1 byte）：PCL Mode 開關，0x00=開、0x01=關（預設）。
//     連線後必須先寫 0x00 開通，裝置才會回應下面的指令；讀完資料務必寫回
//     0x01，不然裝置會停留在鎖定狀態、螢幕顯示 "PCL"（協定文件原文用詞：
//     "the BGM will be locked and display PCL on screen"）——**這代表如果
//     連線在讀取過程中意外斷線（訊號不好、裝置超出範圍等），沒有機會送出
//     這個關閉指令，裝置可能會卡在鎖定畫面，需要使用者自行操作裝置解除
//     （韌體沒有實機驗證過這種異常路徑，見 PROJECT_PLAN.md）。
//   0xFEE2（Notify）：裝置 -> App 的回應通道。單筆回應可能超過一次 BLE
//     Notify 能帶的長度，前面會加 2 bytes 重組表頭，見下面
//     rightest_reassembly_t 的說明。訂閱成功後裝置會自動主動推播一次
//     「meter ID」（協定文件原文："After setting FEE2 Notify, it will
//     automatically return 'meter ID'"），2026-08-28 用 LightBlue 實機確認
//     過推播內容就是裝置自己的序號字串——這不是我們要的回應，收到後直接
//     丟棄，接著送下面的指令。
//   0xFEE3（Write）：App -> 裝置的指令通道。**2026-08-28 用 LightBlue 掃描
//     實機確認：這個 characteristic 只列出 "Write"，沒有 "Write Without
//     Response"**，跟 FEE1（兩者都有）不一樣，一定要用等待 ATT 回應的寫入
//     方式（`gatt_client_write_value_of_characteristic()`），不能像 FORA
//     那樣用 write-without-response。
#define RIGHTEST_SERVICE_UUID16               0xFEE0
#define RIGHTEST_CHARACTERISTIC_PCL_UUID16    0xFEE1
#define RIGHTEST_CHARACTERISTIC_NOTIFY_UUID16 0xFEE2
#define RIGHTEST_CHARACTERISTIC_WRITE_UUID16  0xFEE3

#define RIGHTEST_PCL_MODE_ON  0x00
#define RIGHTEST_PCL_MODE_OFF 0x01

// 指令/回應格式（協定文件 Command/Return Data Format Description）：
//   指令（寫進 FEE3）：0xB0 [CmdID] [Data...] [Checksum]
//   回應（FEE2 Notify 重組後）：0x4F [ReturnID] [Data...] [Checksum]
//   Checksum = 前面所有 byte（含 Header）總和 & 0xFF。
#define RIGHTEST_COMMAND_HEADER 0xB0
#define RIGHTEST_RETURN_HEADER  0x4F

// 查詢型號名稱：0xB0 0x00 CS -> 0x4F 0xFF MN1 MN2 MN3 MN4 MN5 CS（5 個
// ASCII 字元，不保證有結尾 '\0'，協定文件範例是 "GM782"）。這台裝置廣播
// 名稱是序號（協定文件 Architecture Diagram 原文："The BLE advertising
// device name of BGM was serial number of BGM"，2026-08-28 LightBlue 實機
// 確認過），掃描階段沒有型號字串可以比對，只能先靠 Service UUID 0xFEE0
// 粗篩（見 rightest_protocol_matches_advertisement()），真正身份要連線
// 配對、開了 PCL 之後送這個指令，核對回應字串開頭是不是 "GM7" 才能確定
// （見 mode_ble_receive.c 的說明）。
#define RIGHTEST_CMD_QUERY_MODEL_NAME    0x00
#define RIGHTEST_RETURN_QUERY_MODEL_NAME 0xFF
#define RIGHTEST_MODEL_NAME_LEN 5

// 讀記錄：0xB0 0x61 IND_L IND_H CS -> 0x4F 0x9E IND_L IND_H [Data...] CS。
// index=0（TYPE 1）問的是「目前總筆數/裝置最大容量/上次讀到第幾筆（last
// transmission index，裝置自己維護的書籤——協定文件明講每次讀某個 index
// 的記錄，都會把這個書籤推進到那個 index，見 Read One Record 說明）」；
// index>0（TYPE 2）讀第 index 筆的實際記錄內容。**協定文件特別警告**：
// 一定要先送過一次 index=0 的查詢，才能送 index>0 的查詢，不然裝置會回
// 「incorrect or unexpected data」（文件原文）。
//
// 這個「裝置自己記書籤」的設計代表我們不需要像 FORA MD6/D40 backfill 那樣
// 自己在 flash 存一份「上次同步到哪」的定位點（storage.c 的
// storage_get_backfill_anchor()／storage_set_backfill_anchor()）——每次連線
// 只要讀 index=0 拿到 last_transmission_index，從 +1 開始往上讀到
// total_count，讀過的 index 會被裝置自動記住，下次連線的 last_transmission_
// index 自然就是上次讀到的地方，不會重複讀到已經讀過的記錄。前提是這個書籤
// 是裝置端全域狀態、不是每個藍牙連線各自獨立——協定文件沒有明講這點，是從
// 「App 應該從 4th record 開始讀」這種跨 session 接續的範例推斷出來的，
// 2026-08-28 還沒有實機測試驗證過（見 PROJECT_PLAN.md）。
#define RIGHTEST_CMD_READ_RECORD    0x61
#define RIGHTEST_RETURN_READ_RECORD 0x9E

// TYPE 1（index=0）回應：0x4F 0x9E 0x00 0x00 total_lo total_hi max_lo max_hi
// last_lo last_hi CS，共 11 bytes（跟 TYPE 2 的回應長度不同——TYPE 2 多了
// 10 bytes 的 Reserve 欄位，見協定文件 Read One Record 的兩張表格）。
typedef struct {
    uint16_t total_count;             // 裝置目前存有幾筆記錄
    uint16_t max_capacity;            // 裝置最多能存幾筆（協定文件範例 500）
    uint16_t last_transmission_index; // 上次讀到第幾筆的書籤，見上面的說明
} rightest_record_summary_t;

// 把 App 要送出的指令組好（含 checksum）。cmd 是 Command ID，data/data_len
// 是後面接的資料位元組（可以是 0 個、data 傳 NULL），out 至少要有
// data_len+3 bytes 空間，回傳組出來的總長度。
size_t rightest_protocol_build_command(uint8_t cmd, const uint8_t *data, uint8_t data_len, uint8_t *out);

// 驗證一筆已經重組完整的回應 frame：header 是不是 0x4F、checksum 對不對。
bool rightest_protocol_verify_response(const uint8_t *frame, size_t frame_len);

// 解析 TYPE 1（index=0）回應（frame 至少要 11 bytes，header/return-id/
// checksum 都要驗證通過），成功回傳 true。
bool rightest_protocol_parse_record_summary(const uint8_t *frame, size_t frame_len, rightest_record_summary_t *out);

// 解析 TYPE 2（index>0）回應成一筆 vital_record_t，成功回傳 true。
// 回傳 false 的情況：checksum/header/長度不符預期、Hi 旗標（DA_3 bit7=1，
// 代表 >600 mg/dL——協定文件沒講這種情況 Glucose 欄位實際放什麼值，保守
// 起見直接當無效讀值不回傳，不猜）、品管測試（DA_4 bit2=1，比照 FORA QC
// 過濾邏輯，不是病人數值不上傳）。
//
// out->device_measured_key 刻意用跟 fora_protocol.c 的
// fora_protocol_decode_measured_key() 完全相同的 bit-packing 格式
// （(year-2000)<<20 | month<<16 | day<<11 | hour<<6 | minute），這樣
// display_status.c／上傳邏輯既有的 fora_protocol_measured_key_to_datetime()
// /_to_epoch_ms() 可以直接沿用，不用為這台裝置另外寫一份日期換算。
// out->measurement_mode 沿用 fora_protocol.h 的 fora_measurement_mode_t——
// 2026-08-28 使用者決定：GM700SB 的 7 種餐別標記（飯前/飯後/無餐/宵夜/
// 睡前/運動/起床）簡化映射成既有 3 態，飯前->AC、飯後->PC、其餘都當 GEN，
// 不新增欄位／不擴充後端（跟 FORA MD6/D40 的做法一致）。
//
// 每筆記錄自己還帶一個 5-bit 時區欄位（協定文件範例算出來是 GMT-4，
// Meter TZ Index 4=UTC+8 對應台灣），這裡刻意不解析——2026-08-28 使用者
// 決定跟 FORA 一樣直接假設固定台灣時區（LOCAL_UTC_OFFSET_SEC）。
bool rightest_protocol_parse_record(const uint8_t *frame, size_t frame_len, vital_record_t *out);

// 解析型號名稱查詢的回應，把 MN1~MN5 複製進 out 並補上 '\0'（呼叫端保證
// out 至少 RIGHTEST_MODEL_NAME_LEN+1 bytes）。
bool rightest_protocol_parse_model_name(const uint8_t *frame, size_t frame_len, char out[RIGHTEST_MODEL_NAME_LEN + 1]);

// 掃描階段的初步過濾：廣播封包的 16-bit Service UUID 清單裡有沒有列出
// 0xFEE0。這台裝置廣播名稱是序號，沒有型號字串可以比對（見上面
// RIGHTEST_CMD_QUERY_MODEL_NAME 的說明），只能先用這個粗篩；回傳 true
// 不代表確定是 GM700SB（0xFEE0 是 SIG 16-bit UUID 範圍裡的一個值，理論上
// 其他廠牌裝置也可能用到），實際身份要連線配對後送型號查詢指令核對，見
// mode_ble_receive.c。
bool rightest_protocol_matches_advertisement(const uint8_t *adv_data, uint8_t adv_len);

// FEE2 Notify payload 的多包重組緩衝區。單筆回應最長協定文件說約 512
// bytes，超過一次 BLE Notify（受 ATT MTU 限制，扣掉表頭通常 20 bytes 上下）
// 能帶的長度時會拆成多包，每包前面帶 2 bytes 表頭（[總封包數][目前第幾包，
// 從 1 起算]），見協定文件 GATT Payload Format 的例子。
#define RIGHTEST_REASSEMBLY_MAX_BYTES 512
typedef struct {
    uint8_t buffer[RIGHTEST_REASSEMBLY_MAX_BYTES];
    size_t length;
    uint8_t total_packets;
    uint8_t next_packet_index; // 下一個預期收到的封包序號，從 1 起算
    bool in_progress;
} rightest_reassembly_t;

void rightest_reassembly_reset(rightest_reassembly_t *state);

// 餵一個 FEE2 Notify payload 進重組緩衝區。回傳 true 代表這個 payload 收完
// 之後整筆回應剛好重組完整（呼叫端接著讀 state->buffer/state->length 取用，
// 用完要自己呼叫 rightest_reassembly_reset() 準備收下一筆，這個函式本身不會
// 在重組完成後自動重置，讓呼叫端有機會先讀取內容）。回傳 false 代表還在等
// 剩下的封包，或者這個 payload 的表頭跟目前進度對不上（總封包數變了、序號
// 不連續）——這種情況會直接整個重置丟棄，等下一輪從序號 1 重新開始收，避免
// 半筆殘留資料跟下一筆答案混在一起解析出錯誤的內容。
bool rightest_reassembly_feed(rightest_reassembly_t *state, const uint8_t *payload, uint16_t payload_len);

#endif // RIGHTEST_PROTOCOL_H
