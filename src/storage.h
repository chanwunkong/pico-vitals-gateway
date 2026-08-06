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

// 讀取先前儲存的設定；若尚未設定過（flash 是空的）回傳 false。
bool storage_load_config(device_config_t *out);

// 寫入設定並儲存到 flash；回傳是否成功。
bool storage_save_config(const device_config_t *config);

// 新增一筆待傳紀錄；緩衝區滿了會回傳 false（呼叫端應盡快觸發上傳）。
bool storage_append_record(const vital_record_t *record);

// 取出最多 max_count 筆待傳（status == PENDING）的紀錄，回傳實際取出筆數。
size_t storage_pending_records(vital_record_t *out, size_t max_count);

// 將上一次 storage_pending_records() 取出的前 count 筆標記為上傳結果；
// 上傳成功的紀錄會被移除，失敗的保留供下次重試。
void storage_mark_uploaded(size_t count, uint64_t uploaded_at_ms, bool success);

// 查詢某一種生理數值「這次開機以來」最後一次收到的讀值，跟有沒有上傳成功無關
// ——上傳成功的紀錄會從待傳佇列被移除（見 storage_mark_uploaded()），但畫面
// 顯示需要「最後量到多少」這個資訊，所以另外留一份，只存在 RAM，不跨開機
// 持久化（重開機後要等收到新讀值才會再有值，這是刻意的簡化，見
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

#endif // STORAGE_H
