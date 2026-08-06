#include "storage.h"

#include "lfs.h"
#include "lfs_pico_hal.h"

#include <stdio.h>
#include <string.h>

// 設定值/待傳佇列/已上傳歷史各自是 littlefs 分區裡的一個檔案，見 storage.h
// 開頭的說明。
#define CONFIG_FILENAME "config.bin"
#define PENDING_FILENAME "pending.bin"
#define HISTORY_FILENAME "history.bin"

#define MAX_PENDING_RECORDS 128

// 上傳成功後紀錄不會立刻消失，保留最近 MAX_UPLOAD_HISTORY 筆已上傳的紀錄
// （環狀緩衝，滿了覆蓋最舊的一筆），見 storage_get_upload_history()。
#define MAX_UPLOAD_HISTORY 200

static lfs_t s_lfs;
static bool s_lfs_mounted = false;

static vital_record_t s_records[MAX_PENDING_RECORDS];
static size_t s_record_count = 0;

static vital_record_t s_upload_history[MAX_UPLOAD_HISTORY];
static uint32_t s_upload_history_count = 0;
static uint32_t s_upload_history_next = 0;

// 畫面顯示用的「最後一筆讀值」，跟上面待傳佇列分開存——待傳佇列裡的紀錄
// 上傳成功後就會被移除（見 storage_mark_uploaded()），但螢幕仍然需要能顯示
// 「最後量到多少」，見 storage.h 裡 storage_get_last_reading() 的說明。
static vital_record_t s_last_reading[VITAL_TYPE_COUNT];
static bool s_last_reading_valid[VITAL_TYPE_COUNT];

// 最後一次「成功」上傳的時間，畫面顯示用，只存在 RAM、不跨開機持久化（原因
// 跟 s_last_reading 一樣：這只是給人看的參考資訊，真正重要的上傳狀態已經
// 記錄在每筆 vital_record_t 自己的 status/uploaded_at_ms 欄位、且有 flash
// 持久化，這裡不需要重複保存）。
static uint64_t s_last_upload_at_ms;
static bool s_last_upload_valid = false;

// 沒有裝置量測時間戳可用時（device_measured_key 恆為 0 的裝置）的判重備案：
// 數值完全相同、且時間間隔在這個視窗內就當作重複。
#define DUPLICATE_SUPPRESS_WINDOW_MS (10ull * 60 * 1000)

static void persist_pending_records(void);
static void persist_upload_history(void);

// 掛載 littlefs；第一次使用或資料損毀時 lfs_mount() 會失敗，這時候格式化後
// 重新掛載一次。
static bool lfs_mount_or_format(void) {
    int err = lfs_mount(&s_lfs, &lfs_pico_cfg);
    if (err != 0) {
        printf("[STORAGE] lfs_mount() failed (err=%d), formatting flash partition...\n", err);
        int fmt_err = lfs_format(&s_lfs, &lfs_pico_cfg);
        if (fmt_err != 0) {
            printf("[STORAGE] lfs_format() failed (err=%d), storage will be RAM-only this boot.\n", fmt_err);
            return false;
        }
        err = lfs_mount(&s_lfs, &lfs_pico_cfg);
        if (err != 0) {
            printf("[STORAGE] lfs_mount() after format still failed (err=%d), storage will be "
                   "RAM-only this boot.\n", err);
            return false;
        }
    }
    return true;
}

void storage_init(void) {
    s_record_count = 0;
    s_upload_history_count = 0;
    s_upload_history_next = 0;
    memset(s_last_reading_valid, 0, sizeof(s_last_reading_valid));
    s_last_upload_valid = false;

    s_lfs_mounted = lfs_mount_or_format();
    if (!s_lfs_mounted) {
        // 掛載/格式化都失敗（理論上不會發生，除非 flash 硬體真的壞了）——
        // 讓其餘程式碼繼續跑，只是這次開機的資料不會持久化，見上面各個
        // storage_xxx() 函式裡對 s_lfs_mounted 的檢查。
        return;
    }

    // 上次可能是上傳失敗或直接斷電，flash 裡還留著沒傳完的紀錄，讀回來繼續
    // 重試。檔案格式：開頭 4 bytes 是筆數，後面接著實際的 vital_record_t
    // 陣列——不需要像舊版那樣另外存 magic number 判斷「有沒有存過」，
    // lfs_file_open() 開不到檔案本身就代表沒存過。
    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, PENDING_FILENAME, LFS_O_RDONLY) == 0) {
        uint32_t count = 0;
        lfs_ssize_t header_read = lfs_file_read(&s_lfs, &file, &count, sizeof(count));
        if (header_read == (lfs_ssize_t)sizeof(count) && count <= MAX_PENDING_RECORDS) {
            lfs_ssize_t data_read = lfs_file_read(&s_lfs, &file, s_records, count * sizeof(vital_record_t));
            if (data_read == (lfs_ssize_t)(count * sizeof(vital_record_t))) {
                s_record_count = count;
            } else {
                printf("[STORAGE] pending.bin truncated/corrupt (expected %u record(s), "
                       "read %ld bytes), discarding.\n", (unsigned)count, (long)data_read);
            }
        }
        lfs_file_close(&s_lfs, &file);
    }

    if (lfs_file_open(&s_lfs, &file, HISTORY_FILENAME, LFS_O_RDONLY) == 0) {
        uint32_t count = 0, next_index = 0;
        lfs_ssize_t count_read = lfs_file_read(&s_lfs, &file, &count, sizeof(count));
        lfs_ssize_t next_read = lfs_file_read(&s_lfs, &file, &next_index, sizeof(next_index));
        if (count_read == (lfs_ssize_t)sizeof(count) && next_read == (lfs_ssize_t)sizeof(next_index) &&
            count <= MAX_UPLOAD_HISTORY && next_index < MAX_UPLOAD_HISTORY) {
            lfs_ssize_t data_read = lfs_file_read(&s_lfs, &file, s_upload_history, sizeof(s_upload_history));
            if (data_read == (lfs_ssize_t)sizeof(s_upload_history)) {
                s_upload_history_count = count;
                s_upload_history_next = next_index;
            } else {
                printf("[STORAGE] history.bin truncated/corrupt (read %ld bytes), discarding.\n",
                       (long)data_read);
            }
        }
        lfs_file_close(&s_lfs, &file);
    }
}

bool storage_load_config(device_config_t *out) {
    if (!s_lfs_mounted) {
        return false;
    }
    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, CONFIG_FILENAME, LFS_O_RDONLY) != 0) {
        return false; // 檔案不存在，代表還沒設定過
    }
    lfs_ssize_t n = lfs_file_read(&s_lfs, &file, out, sizeof(*out));
    lfs_file_close(&s_lfs, &file);
    return n == (lfs_ssize_t)sizeof(*out);
}

bool storage_save_config(const device_config_t *config) {
    if (!s_lfs_mounted) {
        return false;
    }
    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, CONFIG_FILENAME,
                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
        return false;
    }
    lfs_ssize_t n = lfs_file_write(&s_lfs, &file, config, sizeof(*config));
    int close_err = lfs_file_close(&s_lfs, &file); // close 會 flush 寫入快取
    return n == (lfs_ssize_t)sizeof(*config) && close_err == 0;
}

// 把目前 RAM 裡的待傳清單整份寫回 flash，讓斷電或上傳失敗都不會遺失資料。
static void persist_pending_records(void) {
    if (!s_lfs_mounted) {
        return;
    }
    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, PENDING_FILENAME,
                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
        printf("[STORAGE] persist_pending_records: lfs_file_open failed\n");
        return;
    }
    uint32_t count = (uint32_t)s_record_count;
    lfs_file_write(&s_lfs, &file, &count, sizeof(count));
    lfs_file_write(&s_lfs, &file, s_records, s_record_count * sizeof(vital_record_t));
    lfs_file_close(&s_lfs, &file);
}

// 把目前 RAM 裡的已上傳歷史紀錄整份寫回 flash。
static void persist_upload_history(void) {
    if (!s_lfs_mounted) {
        return;
    }
    lfs_file_t file;
    if (lfs_file_open(&s_lfs, &file, HISTORY_FILENAME,
                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
        printf("[STORAGE] persist_upload_history: lfs_file_open failed\n");
        return;
    }
    lfs_file_write(&s_lfs, &file, &s_upload_history_count, sizeof(s_upload_history_count));
    lfs_file_write(&s_lfs, &file, &s_upload_history_next, sizeof(s_upload_history_next));
    lfs_file_write(&s_lfs, &file, s_upload_history, sizeof(s_upload_history));
    lfs_file_close(&s_lfs, &file);
}

// 上傳成功的紀錄加進環狀歷史緩衝，跟待傳佇列（只保留還沒傳完的）是分開的兩份資料。
static void append_to_upload_history(const vital_record_t *record) {
    s_upload_history[s_upload_history_next] = *record;
    s_upload_history_next = (s_upload_history_next + 1) % MAX_UPLOAD_HISTORY;
    if (s_upload_history_count < MAX_UPLOAD_HISTORY) {
        s_upload_history_count++;
    }
}

bool storage_append_record(const vital_record_t *record) {
    // 重複量測判斷：跟上一筆「同類型、同來源裝置」的最後讀值比對（source_kind
    // 也要相同，見 common.h 的說明，避免不同裝置共用同一個 vital_type_t 時
    // 互相誤判），數值是否完全相同、時間間隔是否在 DUPLICATE_SUPPRESS_WINDOW_MS
    // 之內，是的話視為同一次量測被重複廣播/重新連線收到，不重複塞進待傳佇列。
    // 要在覆寫 s_last_reading 之前比對，不然會拿新紀錄跟自己比。
    bool is_duplicate = false;
    if (record->type < VITAL_TYPE_COUNT && s_last_reading_valid[record->type] &&
        record->source_kind == s_last_reading[record->type].source_kind) {
        const vital_record_t *prev = &s_last_reading[record->type];
        if (record->device_measured_key != 0 && prev->device_measured_key != 0) {
            // 雙方都有裝置認證過的量測時間戳，直接以它為準，不用再猜時間窗口。
            is_duplicate = (record->device_measured_key == prev->device_measured_key);
        } else {
            uint64_t elapsed_ms = record->received_at_ms >= prev->received_at_ms
                ? record->received_at_ms - prev->received_at_ms : 0;
            is_duplicate = (prev->value == record->value && elapsed_ms < DUPLICATE_SUPPRESS_WINDOW_MS);
        }
    }

    // 不論待傳佇列那邊結果如何，畫面顯示用的「最後一筆讀值」一律先更新——就算
    // 判定是重複量測，時間戳照樣往前推進，這樣畫面上才看得出「裝置剛剛還有
    // 確認過這個數值仍然是最新的」，不是凍結在很久以前的舊時間。
    if (record->type < VITAL_TYPE_COUNT) {
        s_last_reading[record->type] = *record;
        s_last_reading_valid[record->type] = true;
    }

    if (is_duplicate) {
        return true;
    }

    // 同一種類型如果已經有一筆還沒上傳成功的舊紀錄（PENDING 或 FAILED），直接
    // 用這筆最新的蓋掉，不要讓同類型的資料一直堆積、上傳好幾筆重複/過時的值
    // （裝置量測完常常會持續廣播一段時間，同一輪可能被連上好幾次）。
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].type == record->type &&
            (s_records[i].status == UPLOAD_STATUS_PENDING || s_records[i].status == UPLOAD_STATUS_FAILED)) {
            s_records[i] = *record;
            persist_pending_records();
            return true;
        }
    }

    if (s_record_count >= MAX_PENDING_RECORDS) {
        return false;
    }
    s_records[s_record_count++] = *record;
    persist_pending_records();
    return true;
}

size_t storage_pending_records(vital_record_t *out, size_t max_count) {
    size_t n = 0;
    for (size_t i = 0; i < s_record_count && n < max_count; i++) {
        // FAILED 也要一起撈出來重試——上次上傳失敗的紀錄如果只挑 PENDING，
        // 就會被永遠卡在陣列裡、再也沒有機會重傳。
        if (s_records[i].status == UPLOAD_STATUS_PENDING || s_records[i].status == UPLOAD_STATUS_FAILED) {
            out[n++] = s_records[i];
        }
    }
    return n;
}

void storage_mark_uploaded(size_t count, uint64_t uploaded_at_ms, bool success) {
    if (success && count > 0) {
        s_last_upload_at_ms = uploaded_at_ms;
        s_last_upload_valid = true;
    }

    size_t marked = 0;
    bool history_changed = false;
    for (size_t i = 0; i < s_record_count && marked < count; i++) {
        // 篩選條件要跟 storage_pending_records() 一致，否則重試批次裡原本是
        // FAILED 的紀錄不會被這裡比對到，結果標記到陣列裡其他不相關的紀錄上。
        if (s_records[i].status == UPLOAD_STATUS_PENDING || s_records[i].status == UPLOAD_STATUS_FAILED) {
            s_records[i].status = success ? UPLOAD_STATUS_UPLOADED : UPLOAD_STATUS_FAILED;
            s_records[i].uploaded_at_ms = uploaded_at_ms;
            marked++;
            if (success) {
                // 上傳成功才留底——失敗的紀錄還留在待傳佇列裡準備重試，不算
                // 「已經處理完的歷史」，見 append_to_upload_history() 的說明。
                append_to_upload_history(&s_records[i]);
                history_changed = true;
            }
        }
    }
    if (history_changed) {
        persist_upload_history();
    }

    // 壓縮陣列：移除已成功上傳的紀錄，保留 PENDING/FAILED 供下次重試。
    size_t write_idx = 0;
    for (size_t i = 0; i < s_record_count; i++) {
        if (s_records[i].status != UPLOAD_STATUS_UPLOADED) {
            s_records[write_idx++] = s_records[i];
        }
    }
    s_record_count = write_idx;
    persist_pending_records();
}

size_t storage_get_upload_history(vital_record_t *out, size_t max_count) {
    // 環狀緩衝，從最舊的一筆開始依序輸出到最新一筆：還沒繞滿一圈之前最舊的
    // 就是 index 0；繞滿一圈之後最舊的是 s_upload_history_next（下一筆要
    // 覆寫的位置，也就是目前最舊的那筆）。
    size_t oldest_index = (s_upload_history_count < MAX_UPLOAD_HISTORY) ? 0 : s_upload_history_next;
    size_t n = 0;
    for (size_t i = 0; i < s_upload_history_count && n < max_count; i++) {
        out[n++] = s_upload_history[(oldest_index + i) % MAX_UPLOAD_HISTORY];
    }
    return n;
}

size_t storage_get_upload_history_count(void) {
    return s_upload_history_count;
}

size_t storage_get_recent_upload_history(vital_record_t *out, size_t max_count) {
    size_t oldest_index = (s_upload_history_count < MAX_UPLOAD_HISTORY) ? 0 : s_upload_history_next;
    size_t n = s_upload_history_count < max_count ? s_upload_history_count : max_count;
    size_t skip = s_upload_history_count - n;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_upload_history[(oldest_index + skip + i) % MAX_UPLOAD_HISTORY];
    }
    return n;
}

bool storage_get_last_reading(vital_type_t type, vital_record_t *out) {
    if (type >= VITAL_TYPE_COUNT || !s_last_reading_valid[type]) {
        return false;
    }
    *out = s_last_reading[type];
    return true;
}

size_t storage_pending_count(void) {
    return s_record_count;
}

bool storage_get_last_upload_time(uint64_t *out_ms) {
    if (!s_last_upload_valid) {
        return false;
    }
    *out_ms = s_last_upload_at_ms;
    return true;
}
