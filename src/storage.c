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

static vital_record_t s_records[MAX_PENDING_RECORDS];
static size_t s_record_count = 0;

typedef struct {
    const void *data;
    size_t size;
} flash_write_params_t;

static void flash_erase_and_program(void *param) {
    flash_write_params_t *p = (flash_write_params_t *)param;
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, (const uint8_t *)p->data, p->size);
}

void storage_init(void) {
    s_record_count = 0;
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

    flash_write_params_t params = { .data = page_buf, .size = sizeof(page_buf) };
    int rc = flash_safe_execute(flash_erase_and_program, &params, 1000);
    return rc == PICO_OK;
}

bool storage_append_record(const vital_record_t *record) {
    if (s_record_count >= MAX_PENDING_RECORDS) {
        return false;
    }
    s_records[s_record_count++] = *record;
    return true;
}

size_t storage_pending_records(vital_record_t *out, size_t max_count) {
    size_t n = 0;
    for (size_t i = 0; i < s_record_count && n < max_count; i++) {
        if (s_records[i].status == UPLOAD_STATUS_PENDING) {
            out[n++] = s_records[i];
        }
    }
    return n;
}

void storage_mark_uploaded(size_t count, uint64_t uploaded_at_ms, bool success) {
    size_t marked = 0;
    for (size_t i = 0; i < s_record_count && marked < count; i++) {
        if (s_records[i].status == UPLOAD_STATUS_PENDING) {
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
}
