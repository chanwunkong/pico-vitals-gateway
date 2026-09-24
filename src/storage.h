#ifndef STORAGE_H
#define STORAGE_H

#include "common.h"
#include <stddef.h>

// 設定值、待傳生理資料清單、已上傳歷史都持久化在 littlefs 掛載的一個 flash
// 分區裡（各自是分區裡的一個檔案：config.bin／pending.bin／history.bin，見
// lfs_pico_hal.c/.h），抹寫會在分區內的多個 block 之間輪替。
//
// 生理資料（vital_record_t）待傳清單會整份寫回 flash，每次新增/標記上傳
// 結果都整份覆寫一次，所以斷電或上傳失敗都不會遺失尚未成功上傳的資料，
// 重開機會自動讀回繼續重試。
//
// 上傳成功的紀錄不會立刻從本機消失：另外用一個環狀緩衝（最近
// MAX_UPLOAD_HISTORY 筆，見 storage.c）保留一份已上傳歷史，一樣持久化在
// 同一個分區裡，供之後查驗/除錯用，見 storage_get_upload_history()。

void storage_init(void);

// 開機時按住 KEY2 觸發的「清空重來」，把 littlefs 分區整個重新格式化——見
// storage.c 的說明。**要在 storage_init() 之前呼叫**，main.c 負責檢查按鍵。
void storage_factory_reset(void);

// 讀取先前儲存的設定；若尚未設定過（flash 是空的）回傳 false。
bool storage_load_config(device_config_t *out);

// 寫入設定並儲存到 flash；回傳是否成功。
bool storage_save_config(const device_config_t *config);

// 新增一筆待傳紀錄；緩衝區滿了會回傳 false（呼叫端應盡快觸發上傳）。
bool storage_append_record(const vital_record_t *record);

// 取出最多 max_count 筆待傳（status == PENDING）的紀錄，回傳實際取出筆數。
size_t storage_pending_records(vital_record_t *out, size_t max_count);

// 取出待傳（PENDING 或 FAILED）紀錄的其中一頁：跳過前 skip 筆符合條件的紀錄，
// 再取最多 max_count 筆，回傳實際取出筆數。跟 storage_pending_records() 的
// 差別是這個給 KEY2 歷史畫面翻頁用——畫面一次只需要一頁（例如 7 筆），不需要
// 呼叫端準備能裝下全部（最多 MAX_PENDING_RECORDS 筆）的 buffer。
size_t storage_pending_records_page(vital_record_t *out, size_t max_count, size_t skip);

// 將上一次 storage_pending_records() 取出的紀錄裡、來源裝置種類是
// source_kind「且」device_measured_key 也相同的那些（PENDING 或 FAILED）
// 標記為上傳結果；上傳成功的紀錄會被移除，失敗的保留供下次重試，其他來源/
// 其他時間點的紀錄不受影響。後端一次上傳是一筆彙整過的紀錄、一個時間點只能
// 有一個值（見 upload_api.c 的說明），mode_upload.c 依 (source_kind,
// device_measured_key) 分組個別呼叫上傳 API 之後，要能只標記「剛剛那組」的
// 結果，不能影響到陣列裡其他組別、還沒處理過的紀錄——這對額溫槍/血氧計這種
// 沒有裝置時間戳的裝置沒有影響（device_measured_key 恆為 0，等同只依
// source_kind 分組，跟這個函式原本的行為一樣）；對血壓計/MD6 這種一次連線
// 可能抓到好幾個不同時間點記錄的裝置，才會真的按時間點分開標記。
void storage_mark_uploaded_for_group(
    uint8_t source_kind, uint32_t device_measured_key, uint64_t uploaded_at_ms, bool success);

// 查詢某一種生理數值「這次開機以來」最後一次收到的讀值，跟有沒有上傳成功無關
// ——上傳成功的紀錄會從待傳佇列被移除（見 storage_mark_uploaded_for_group()），
// 但畫面顯示需要「最後量到多少」這個資訊，所以另外留一份，只存在 RAM，不跨
// 開機持久化（重開機後要等收到新讀值才會再有值，這是刻意的簡化，見
// PROJECT_PLAN.md 第 12.6 節）。回傳 false 代表這次開機還沒收過這種類型。
bool storage_get_last_reading(vital_type_t type, vital_record_t *out);

// 目前待上傳（PENDING 或 FAILED）的紀錄筆數，畫面顯示用。
size_t storage_pending_count(void);

// 查詢「這次開機以來」最後一次成功上傳的時間（跟 storage_append_record()
// 的 received_at_ms 是同一種時間基準——mode_upload.c 目前傳進來的一律是
// boot-relative ms，不是換算過的真實世界時間）。回傳 false 代表這次開機
// 還沒有成功上傳過。
bool storage_get_last_upload_time(uint64_t *out_ms);

// 取出最近上傳成功的歷史紀錄，最舊的排在前面，最新的排在最後，最多取
// max_count 筆，回傳實際取出筆數。見 storage.h 開頭「上傳成功的紀錄不會
// 立刻從本機消失」的說明——這份資料獨立於待傳佇列之外，紀錄一旦上傳成功
// 就會同時進到這裡，滿了（MAX_UPLOAD_HISTORY 筆）會覆蓋最舊的一筆。
size_t storage_get_upload_history(vital_record_t *out, size_t max_count);

// 已上傳歷史目前總共有幾筆（跟 storage_pending_count() 是兩份獨立的計數，
// 這個只計已上傳成功、進到 storage_get_upload_history() 那份環狀緩衝的）。
size_t storage_get_upload_history_count(void);

// 取出最近上傳成功的「最新」max_count 筆（跟 storage_get_upload_history()
// 從最舊開始取不同，這個是從最新往回取），一樣最舊的排前面、最新的排最後，
// 回傳實際取出筆數。畫面顯示「最近幾筆上傳紀錄」用。
size_t storage_get_recent_upload_history(vital_record_t *out, size_t max_count);

// 血壓計/MD6/GM700SB backfill 用的「上次同步到哪一筆」定位點：8 bytes 是
// 那個裝置最新一筆記錄的原始內容（不是解析過的數值，也不是裝置時間戳——
// 裝置時鐘不可信任，改成直接比對原始 bytes 是不是同一筆）。2026-09-23 從
// 「每個裝置種類一份」改成「每個(裝置種類, 藍牙位址)各自一份」——同一種類
// 如果輪流接不同實機（例如血糖機換新的、或同一台 gateway 服務多位病患各自
// 的血糖機），原本只認裝置種類的定位點會被最後連線過的那台覆蓋，導致其他
// 台每次重連都被誤判成「全部沒同步過」重新整批上傳一次。source_kind 當
// 不透明的 uint8_t 用，跟 vital_record_t.source_kind 是同一組值
// （fora_device_kind_t）；device_addr 是 BTstack 的 bd_addr_t（6 bytes）。
// storage_get_backfill_anchor() 回傳 false 代表這個(種類,位址)組合還沒有
// 存過定位點（例如第一次連線這台裝置）。每組定位點各自存一個獨立小檔案
// （見 storage.c），能同時追蹤幾台不受陣列大小限制，只受 flash 剩餘空間
// 限制。
bool storage_get_backfill_anchor(uint8_t source_kind, const uint8_t device_addr[6], uint8_t out_anchor[8]);
void storage_set_backfill_anchor(uint8_t source_kind, const uint8_t device_addr[6], const uint8_t anchor[8]);

// 個案換人時呼叫，把所有裝置的 backfill 同步點全部清掉——同步點是依
// (裝置種類,藍牙位址) 存的，跟個案無關，換個案卻不清的話，舊個案用過的裝置
// 如果被拿來給新個案繼續用，會讓 mode_ble_receive.c 的「沒有既有同步點才
// 保守處理」保底機制被繞過，見該檔案 should_limit_backfill_to_latest_only()
// 的說明。
void storage_clear_all_backfill_anchors(void);

#endif // STORAGE_H
