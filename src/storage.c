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
