#ifndef STORAGE_H
#define STORAGE_H

#include "common.h"
#include <stddef.h>

// 設定資料存在 flash 最後一個保留 sector，寫入透過 flash_safe_execute() 執行，
// 讀取直接用 XIP 位址存取（讀取不需要暫停中斷）。
//
// 生理資料（vital_record_t）待傳清單同樣會整份寫回 flash（config 前面再保留
// 幾個 sector），每次新增/標記上傳結果都整份覆寫一次，所以斷電或上傳失敗
// 都不會遺失尚未成功上傳的資料，重開機會自動讀回繼續重試。
// 這個做法沒有 wear leveling，寫入頻率高的話會較快耗損那幾個 sector；
// 正式量產前如果讀值頻率提高，需評估升級成 littlefs，見 PROJECT_PLAN.md 第7節。

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

#endif // STORAGE_H
