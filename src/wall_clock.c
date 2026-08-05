#include "wall_clock.h"

#include "lwip/apps/sntp.h"
#include "pico/time.h"

#include <stdio.h>

// 2026-08-05 從 24 小時改成 6 小時：RP2040 內建震盪器的漂移量級是一天幾秒，
// 不需要每次上傳都重打一次 NTP（那樣每次上傳都多一次網路來回，卻幾乎沒有
// 實質收穫），但 6 小時一次的頻率負擔還是很小——只是趁 WiFi 已經連線時順便
// 打一個很小的 UDP 封包，不會額外開一次 WiFi 連線，比 24 小時多留一點餘裕
// 應付長時間連續運作的漂移累積。開機後第一次呼叫（從沒成功校時過）一定會
// 真的去打，之後只有距離上次成功校時超過這個間隔才會真的重新查詢，其餘時間
// 直接沿用已經校過的基準點。
#define NTP_RESYNC_INTERVAL_MS (6ULL * 60 * 60 * 1000)

// 依常見程度排序輪流嘗試，其中一個沒回應（DNS 解不出來/逾時）就換下一個，
// 不要單一伺服器打不通就整批直接放棄校時（做法比照 mode_upload.c 的
// WIFI_AUTH_MODES_TO_TRY 容錯模式）。
static const char *NTP_SERVERS_TO_TRY[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
};
#define NTP_SERVER_COUNT (sizeof(NTP_SERVERS_TO_TRY) / sizeof(NTP_SERVERS_TO_TRY[0]))

// s_callback_fired 是「這次嘗試有沒有收到新回應」，每次嘗試前會重置；
// s_have_basis 是「開機以來有沒有成功校時過至少一次」，一旦變 true 就不會
// 再變回 false——就算之後的重新校時嘗試全部失敗，還是要繼續用上一次成功
// 校時建立的基準點換算時間，好過完全放棄、退回沒有意義的 boot-relative 值。
static volatile bool s_callback_fired = false;
static bool s_have_basis = false;
static uint64_t s_sync_epoch_ms = 0;
static uint64_t s_sync_boot_ms = 0;

// lwipopts.h 把這個函式接到 SNTP_SET_SYSTEM_TIME_US 巨集，lwIP 的 sntp.c 收到
// NTP 伺服器回應時會直接呼叫。sec/us 是 NTP 回應換算出來的 UTC epoch 時間。
void wall_clock_sntp_set_system_time_us(uint32_t sec, uint32_t us) {
    s_sync_epoch_ms = (uint64_t)sec * 1000u + us / 1000u;
    s_sync_boot_ms = to_ms_since_boot(get_absolute_time());
    s_have_basis = true;
    s_callback_fired = true;
    printf("[TIME] NTP synced, epoch_ms=%llu\n", (unsigned long long)s_sync_epoch_ms);
}

static bool try_one_server(const char *server, uint32_t timeout_ms) {
    s_callback_fired = false;
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, server);
    sntp_init();

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!s_callback_fired && !time_reached(deadline)) {
        sleep_ms(50);
    }
    sntp_stop();
    return s_callback_fired;
}

bool wall_clock_sync(uint32_t timeout_ms) {
    if (s_have_basis) {
        uint64_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - s_sync_boot_ms < NTP_RESYNC_INTERVAL_MS) {
            // 距離上次成功校時還不到 24 小時，沿用現有基準點，不用再打網路。
            // 這裡故意還是印一行 log——這個分支完全不打網路，如果不印任何
            // 東西，從 log 完全看不出「時間到底有沒有校過」，之前排查上傳
            // 問題時就因為這樣誤以為時間校時本身失敗了，其實只是校過之後
            // 每次都安靜地跳過而已。
            printf("[TIME] using existing sync from %llums ago (epoch_ms=%llu)\n",
                   (unsigned long long)(now_ms - s_sync_boot_ms), (unsigned long long)s_sync_epoch_ms);
            return true;
        }
        printf("[TIME] last NTP sync was >=24h ago, refreshing...\n");
    }

    uint32_t per_server_timeout_ms = (uint32_t)(timeout_ms / NTP_SERVER_COUNT);
    if (per_server_timeout_ms == 0) {
        per_server_timeout_ms = timeout_ms; // 呼叫端給的預算太短，就別再切了，整包用上
    }

    for (size_t i = 0; i < NTP_SERVER_COUNT; i++) {
        printf("[TIME] trying NTP server \"%s\"...\n", NTP_SERVERS_TO_TRY[i]);
        if (try_one_server(NTP_SERVERS_TO_TRY[i], per_server_timeout_ms)) {
            return true;
        }
    }

    if (s_have_basis) {
        // 已經有一份（現在偏舊的）基準點，這次重新校時失敗不代表要整個放棄
        // 換算時間——繼續用舊的基準點換算，比完全退回 boot-relative 好，下次
        // 上傳連上網路時還會再試一次。
        printf("[TIME] refresh failed on all servers, keeping previous (now stale) time basis.\n");
        return true;
    }

    printf("[TIME] NTP sync timed out on all servers, using boot-relative timestamps for this batch.\n");
    return false;
}

bool wall_clock_is_synced(void) {
    return s_have_basis;
}

uint64_t wall_clock_to_epoch_ms(uint64_t boot_ms) {
    if (!s_have_basis) {
        return boot_ms;
    }
    // boot_ms 可能是校時之前拍下的（量測發生在校時前），也可能之後——兩種都是
    // 用「校時那一刻的 boot ms 對應到哪個 epoch ms」直接線性平移，跟先後順序無關。
    return s_sync_epoch_ms + (boot_ms - s_sync_boot_ms);
}
