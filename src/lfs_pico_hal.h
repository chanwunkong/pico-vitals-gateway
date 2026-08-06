#ifndef LFS_PICO_HAL_H
#define LFS_PICO_HAL_H

#include "lfs.h"

// littlefs 掛載在 flash 最後 LFS_PICO_PARTITION_SIZE bytes 劃出來的一個分區。
#define LFS_PICO_PARTITION_SIZE (256 * 1024)

// littlefs 的 lfs_config，storage.c 呼叫 lfs_mount()/lfs_format() 時使用。
// 讀寫/抹除的實際實作見 lfs_pico_hal.c：讀取直接用 XIP 位址（不需要暫停
// 中斷），寫入/抹除透過 flash_safe_execute() 安全執行（RP2040 抹寫 flash
// 期間 XIP 無法使用，必須確保這段期間沒有其他程式碼透過 XIP 執行/讀取）。
extern const struct lfs_config lfs_pico_cfg;

#endif // LFS_PICO_HAL_H
