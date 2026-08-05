#include "storage.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"

#include <string.h>

#define CONFIG_MAGIC 0x50494B31u // "PIK1"
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    device_config_t config;
} config_flash_block_t;

#define CONFIG_PAGE_BYTES (FLASH_PAGE_SIZE * \
    ((sizeof(config_flash_block_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE))

#define MAX_PENDING_RECORDS 128

#define PENDING_MAGIC 0x50494B32u // "PIK2"

typedef struct {
    uint32_t magic;
    uint32_t count;
    vital_record_t records[MAX_PENDING_RECORDS];
} pending_flash_block_t;

#define PENDING_PAGE_BYTES (FLASH_PAGE_SIZE * \
    ((sizeof(pending_flash_block_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE))
#define PENDING_FLASH_SECTORS \
    ((PENDING_PAGE_BYTES + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE)
#define PENDING_FLASH_SIZE (PENDING_FLASH_SECTORS * FLASH_SECTOR_SIZE)
// 保留在 config 那個 sector 前面，跟 config 分開各自一塊，互不影響。
#define PENDING_FLASH_OFFSET (CONFIG_FLASH_OFFSET - PENDING_FLASH_SIZE)

static vital_record_t s_records[MAX_PENDING_RECORDS];
static size_t s_record_count = 0;

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

// 血壓計（理論上額溫槍/血氧計也可能）量測完會持續廣播一段時間，裝置本身不會
// 標記「這筆記錄已經被讀走」——冷卻時間（見 mode_ble_receive.c 的
// DEVICE_RECONNECT_COOLDOWN_MS）一到就會重新連線，每次都拿到一模一樣的「目前
// 最新一筆」記錄。2026-08-05 實測證實：同一次量測在 35 秒左右的重連週期下，
// 被當成 3 筆不同紀錄重複上傳到伺服器（見 PROJECT_PLAN.md 第 6.3 節）。
//
// 血壓計的記錄本身帶有裝置自己認證過的量測時間戳（見
// vital_record_t.device_measured_key），這種情況下直接比對時間戳是否相等，
// 不需要猜時間窗口——同一個時間戳保證是同一筆記錄，不同時間戳保證是不同筆
// （裝置量測本身要 30-45 秒，同一分鐘不可能量出兩筆）。只有在協定沒有提供
// 時間戳的裝置（額溫槍/血氧計，device_measured_key 恆為 0）才退回用這個
// 經驗法則：數值完全相同、且時間間隔在這個視窗內就當作重複。設 10 分鐘：
// 遠大於觀察到的重連週期，足以蓋過裝置整段廣播期間；如果同一種類型真的在
// 10 分鐘內量出完全相同的數值，會被誤判成重複而漏傳一筆，但比起現況（同一筆
// 量測值無限重複灌進伺服器）是更好的取捨。
#define DUPLICATE_SUPPRESS_WINDOW_MS (10ull * 60 * 1000)

static void persist_pending_records(void);

typedef struct {
    uint32_t flash_offset;
    uint32_t erase_size;
    const void *data;
    size_t data_size;
} flash_write_params_t;

static void flash_erase_and_program(void *param) {
    flash_write_params_t *p = (flash_write_params_t *)param;
    flash_range_erase(p->flash_offset, p->erase_size);
    flash_range_program(p->flash_offset, (const uint8_t *)p->data, p->data_size);
}

void storage_init(void) {
    s_record_count = 0;
    memset(s_last_reading_valid, 0, sizeof(s_last_reading_valid));
    s_last_upload_valid = false;

    const pending_flash_block_t *stored =
        (const pending_flash_block_t *)(XIP_BASE + PENDING_FLASH_OFFSET);
    if (stored->magic == PENDING_MAGIC && stored->count <= MAX_PENDING_RECORDS) {
        // 上次可能是上傳失敗或直接斷電，flash 裡還留著沒傳完的紀錄，讀回來繼續重試。
        memcpy(s_records, stored->records, stored->count * sizeof(vital_record_t));
        s_record_count = stored->count;
    }
}

bool storage_load_config(device_config_t *out) {
    const config_flash_block_t *stored =
        (const config_flash_block_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);

    if (stored->magic != CONFIG_MAGIC) {
        return false; // flash 是空的 (0xFFFFFFFF)，代表還沒設定過
    }

    memcpy(out, &stored->config, sizeof(device_config_t));
    return true;
}

bool storage_save_config(const device_config_t *config) {
    static uint8_t page_buf[CONFIG_PAGE_BYTES];
    memset(page_buf, 0xFF, sizeof(page_buf));

    config_flash_block_t block;
    block.magic = CONFIG_MAGIC;
    memcpy(&block.config, config, sizeof(device_config_t));
    memcpy(page_buf, &block, sizeof(block));

    flash_write_params_t params = {
        .flash_offset = CONFIG_FLASH_OFFSET,
        .erase_size = FLASH_SECTOR_SIZE,
        .data = page_buf,
        .data_size = sizeof(page_buf),
    };
    int rc = flash_safe_execute(flash_erase_and_program, &params, 1000);
    return rc == PICO_OK;
}

// 把目前 RAM 裡的待傳清單整份寫回 flash，讓斷電或上傳失敗都不會遺失資料。
// 沒有 wear leveling，每次呼叫都會整份覆寫——見 storage.h 開頭的取捨說明。
static void persist_pending_records(void) {
    static uint8_t page_buf[PENDING_PAGE_BYTES];
    memset(page_buf, 0xFF, sizeof(page_buf));

    // page_buf 直接當成 pending_flash_block_t 寫，省掉再多開一份跟它一樣大的
    // 區域變數（這個 struct 含 128 筆 vital_record_t，放堆疊上太浪費）。
    pending_flash_block_t *block = (pending_flash_block_t *)page_buf;
    block->magic = PENDING_MAGIC;
    block->count = (uint32_t)s_record_count;
    memcpy(block->records, s_records, s_record_count * sizeof(vital_record_t));

    flash_write_params_t params = {
        .flash_offset = PENDING_FLASH_OFFSET,
        .erase_size = PENDING_FLASH_SIZE,
        .data = page_buf,
        .data_size = sizeof(page_buf),
    };
    flash_safe_execute(flash_erase_and_program, &params, 1000);
}

bool storage_append_record(const vital_record_t *record) {
    // 重複量測判斷：跟上一筆同類型的「最後讀值」比對數值是否完全相同、時間間隔
    // 是否在 DUPLICATE_SUPPRESS_WINDOW_MS 之內，是的話視為同一次量測被重複
    // 廣播/重新連線收到，不重複塞進待傳佇列（見上面常數的說明）。要在覆寫
    // s_last_reading 之前比對，不然會拿新紀錄跟自己比。
    // 2026-08-05 加上 source_kind 比對：VITAL_TYPE_PULSE_RATE 同時被血氧計跟
    // 血壓計共用（見 common.h vital_record_t.source_kind 的說明），如果只比對
    // vital_type_t、不管是哪種裝置量的，會把「血氧計這次的脈搏」誤判成要跟
    // 「血壓計上次回報的脈搏」比較——兩種裝置的數值只要剛好相同，血氧計真正
    // 的新讀值就會被誤判成重複而漏傳（比多傳一筆更嚴重的方向：這是真的會遺失
    // 資料的方向，2026-08-05 實測抓到過一次症狀，見 PROJECT_PLAN.md 第 6.3
    // 節）。不同裝置種類回報同一種 vital_type_t 一律當作不同來源，不比對、
    // 直接視為新資料。
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
    for (size_t i = 0; i < s_record_count && marked < count; i++) {
        // 篩選條件要跟 storage_pending_records() 一致，否則重試批次裡原本是
        // FAILED 的紀錄不會被這裡比對到，結果標記到陣列裡其他不相關的紀錄上。
        if (s_records[i].status == UPLOAD_STATUS_PENDING || s_records[i].status == UPLOAD_STATUS_FAILED) {
            s_records[i].status = success ? UPLOAD_STATUS_UPLOADED : UPLOAD_STATUS_FAILED;
            s_records[i].uploaded_at_ms = uploaded_at_ms;
            marked++;
        }
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
