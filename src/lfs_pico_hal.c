#include "lfs_pico_hal.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"

#include <string.h>

// littlefs 分區劃在 flash 最後面，大小見 lfs_pico_hal.h 的
// LFS_PICO_PARTITION_SIZE。
#define LFS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - LFS_PICO_PARTITION_SIZE)

// 一個 littlefs block 對應剛好一個實體 flash sector。
#define LFS_BLOCK_SIZE FLASH_SECTOR_SIZE
#define LFS_BLOCK_COUNT (LFS_PICO_PARTITION_SIZE / LFS_BLOCK_SIZE)

// prog/cache 大小對齊 flash_range_program() 要求的 256-byte 邊界。
#define LFS_PROG_SIZE FLASH_PAGE_SIZE
#define LFS_READ_SIZE 16
#define LFS_CACHE_SIZE LFS_PROG_SIZE
#define LFS_LOOKAHEAD_SIZE 16

// flash_safe_execute() 要求傳進去的函式簽名是 void (*)(void*)，這裡包一層
// 轉呼叫實際的 flash_range_erase/program。
typedef struct {
    uint32_t flash_offset;
    const void *data;
    size_t size;
} lfs_pico_prog_params_t;

static void lfs_pico_erase_op(void *param) {
    uint32_t offset = *(const uint32_t *)param;
    flash_range_erase(offset, LFS_BLOCK_SIZE);
}

static void lfs_pico_prog_op(void *param) {
    const lfs_pico_prog_params_t *p = (const lfs_pico_prog_params_t *)param;
    flash_range_program(p->flash_offset, (const uint8_t *)p->data, p->size);
}

static int lfs_pico_read(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;
    // 讀取直接用 XIP 記憶體對映位址存取，不需要暫停中斷。
    uint32_t addr = LFS_FLASH_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE + off;
    memcpy(buffer, (const void *)(XIP_BASE + addr), size);
    return 0;
}

static int lfs_pico_prog(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = LFS_FLASH_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE + off;
    lfs_pico_prog_params_t params = { .flash_offset = addr, .data = buffer, .size = size };
    int rc = flash_safe_execute(lfs_pico_prog_op, &params, 1000);
    return rc == PICO_OK ? 0 : LFS_ERR_IO;
}

static int lfs_pico_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    uint32_t addr = LFS_FLASH_OFFSET + (uint32_t)block * LFS_BLOCK_SIZE;
    int rc = flash_safe_execute(lfs_pico_erase_op, &addr, 1000);
    return rc == PICO_OK ? 0 : LFS_ERR_IO;
}

static int lfs_pico_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

// 靜態配置讀寫/lookahead 緩衝區，不使用 littlefs 內建的 lfs_malloc()。
static uint8_t s_lfs_read_buffer[LFS_CACHE_SIZE];
static uint8_t s_lfs_prog_buffer[LFS_CACHE_SIZE];
static uint8_t s_lfs_lookahead_buffer[LFS_LOOKAHEAD_SIZE];

const struct lfs_config lfs_pico_cfg = {
    .read = lfs_pico_read,
    .prog = lfs_pico_prog,
    .erase = lfs_pico_erase,
    .sync = lfs_pico_sync,

    .read_size = LFS_READ_SIZE,
    .prog_size = LFS_PROG_SIZE,
    .block_size = LFS_BLOCK_SIZE,
    .block_count = LFS_BLOCK_COUNT,
    .block_cycles = 500,

    .cache_size = LFS_CACHE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE,

    .read_buffer = s_lfs_read_buffer,
    .prog_buffer = s_lfs_prog_buffer,
    .lookahead_buffer = s_lfs_lookahead_buffer,
};
