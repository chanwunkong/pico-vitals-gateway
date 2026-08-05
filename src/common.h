#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>

// 這個專案目前只在台灣使用，統一假設「本地時間」是 UTC+8——wall_clock 校時後
// 換算真人看得懂的時鐘（display_status.c）、跟血壓計自己內部時鐘的量測時間
// 換算成 epoch ms（fora_protocol.c 的 fora_protocol_measured_key_to_epoch_ms()）
// 都要用同一個時區假設，寫在這裡共用一份，避免兩邊各自寫一份常數、之後改一邊
// 忘記改另一邊。
#define LOCAL_UTC_OFFSET_SEC (8 * 3600)

#define WIFI_SSID_MAX_LEN    32
#define WIFI_PASS_MAX_LEN    64
#define PATIENT_NAME_MAX_LEN 64
#define PATIENT_ID_MAX_LEN   32
#define CASE_MANAGER_MAX_LEN 64

// 熱點設定模式收集的裝置設定。
typedef struct {
    char wifi_ssid[WIFI_SSID_MAX_LEN];
    char wifi_password[WIFI_PASS_MAX_LEN];
    char patient_name[PATIENT_NAME_MAX_LEN];
    char patient_id[PATIENT_ID_MAX_LEN];
    char case_manager_info[CASE_MANAGER_MAX_LEN];
    bool valid;
} device_config_t;

typedef enum {
    VITAL_TYPE_UNKNOWN = 0,
    VITAL_TYPE_TEMPERATURE,
    VITAL_TYPE_SPO2,
    VITAL_TYPE_PULSE_RATE,
    VITAL_TYPE_SYSTOLIC,
    VITAL_TYPE_DIASTOLIC,
    // 2026-08-05 先保留欄位：FORA D40 是血壓血糖二合一裝置，血糖部分還沒實作
    // （協定還沒確認，見 PROJECT_PLAN.md 第 7 節第 2 點），但畫面/資料型別先
    // 把欄位留著，之後接上血糖協定時不用再改動 storage/display 這幾層。
    VITAL_TYPE_GLUCOSE,
    VITAL_TYPE_COUNT, // 型別數量，不是實際的量測類型，只用來宣告陣列大小
} vital_type_t;

typedef enum {
    UPLOAD_STATUS_PENDING = 0,
    UPLOAD_STATUS_UPLOADED,
    UPLOAD_STATUS_FAILED,
} upload_status_t;

// 一筆生理量測紀錄。
typedef struct {
    uint64_t received_at_ms;
    uint64_t uploaded_at_ms;
    vital_type_t type;
    float value;
    upload_status_t status;
    // 裝置自己回報的量測時間（分鐘解析度，編碼方式見 fora_protocol.h 的
    // fora_protocol_parse_reading() 說明），0 代表這種裝置的協定沒有這個資訊
    // （目前額溫槍/血氧計是 0，血壓計有值）。storage.c 判斷是不是同一次量測
    // 被重複廣播/重連收到時，這個欄位有值就以它為準（同一個時間戳＝裝置認證
    // 過的同一筆記錄，不是靠猜的），只有在雙方都是 0（沒有這個資訊可比對）
    // 才退回用「數值相同+短時間內」的經驗法則，見 storage_append_record()。
    // 只在 RAM 裡比對用，不會被序列化進上傳的 JSON（build_request() 沒有用到
    // 這個欄位）、也不需要 flash 持久化格式相容性考量。
    uint32_t device_measured_key;
    // 是哪一種裝置回報的這筆讀值（實際數值範圍/意義由對應的協定模組定義，
    // 目前是 fora_protocol.h 的 fora_device_kind_t，這裡故意用不透明的
    // uint8_t 而不是直接引用那個型別，避免 common.h 反過來依賴
    // fora_protocol.h 造成循環 include）。
    //
    // 2026-08-05 加這個欄位的原因：VITAL_TYPE_PULSE_RATE 這個 vital_type_t
    // 同時被血氧計跟血壓計共用（兩種裝置都會回報脈搏），storage.c
    // 判斷「是不是同一次量測被重複收到」時，如果只比對 vital_type_t 不看是
    // 哪種裝置量的，會把「血氧計這次的脈搏」誤判成「跟血壓計上次回報的脈搏
    // 比較」，兩種裝置剛好量到同一個數字時會把血氧計真正的新讀值誤判成重複
    // 而漏傳（比多傳一筆更嚴重的方向）。storage_append_record() 判重時要求
    // source_kind 也要相同才能算重複，見該函式的說明。
    uint8_t source_kind;
} vital_record_t;

#endif // COMMON_H
