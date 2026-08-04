#include "wall_clock.h"

#include "lwip/apps/sntp.h"
#include "pico/time.h"

#include <stdio.h>

static volatile bool s_synced = false;
static uint64_t s_sync_epoch_ms = 0;
static uint64_t s_sync_boot_ms = 0;

// lwipopts.h 把這個函式接到 SNTP_SET_SYSTEM_TIME_US 巨集，lwIP 的 sntp.c 收到
// NTP 伺服器回應時會直接呼叫。sec/us 是 NTP 回應換算出來的 UTC epoch 時間。
void wall_clock_sntp_set_system_time_us(uint32_t sec, uint32_t us) {
    s_sync_epoch_ms = (uint64_t)sec * 1000u + us / 1000u;
    s_sync_boot_ms = to_ms_since_boot(get_absolute_time());
    s_synced = true;
    printf("[TIME] NTP synced, epoch_ms=%llu\n", (unsigned long long)s_sync_epoch_ms);
}

bool wall_clock_sync(uint32_t timeout_ms) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!s_synced && !time_reached(deadline)) {
        sleep_ms(50);
    }

    sntp_stop();
    if (!s_synced) {
        printf("[TIME] NTP sync timed out, using boot-relative timestamps for this batch.\n");
    }
    return s_synced;
}

bool wall_clock_is_synced(void) {
    return s_synced;
}

uint64_t wall_clock_to_epoch_ms(uint64_t boot_ms) {
    if (!s_synced) {
        return boot_ms;
    }
    // boot_ms 可能是校時之前拍下的（量測發生在校時前），也可能之後——兩種都是
    // 用「校時那一刻的 boot ms 對應到哪個 epoch ms」直接線性平移，跟先後順序無關。
    return s_sync_epoch_ms + (boot_ms - s_sync_boot_ms);
}
