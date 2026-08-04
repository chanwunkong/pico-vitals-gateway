#include "mode_upload.h"

#include "common.h"
#include "led_status.h"
#include "storage.h"
#include "upload_api.h"
#include "wall_clock.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/netif.h"

#include <stdio.h>

// 曾經卡在「DHCP 明明成功拿到 IP，卻被誤判逾時、整個連線被砍掉重建」的 bug
// （見下面用 netif 直接檢查 IP 而非只信任 cyw43_wifi_link_status() 的邏輯），
// 30 秒是留給 DHCP 重試（實測最壞情況約 15-16 秒）+ 認證本身的餘裕。
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define MAX_BATCH_SIZE 32

// 認證模式不寫死——分享器種類很多，依常見程度排序嘗試，直到成功或全部試完。
static const uint32_t WIFI_AUTH_MODES_TO_TRY[] = {
    CYW43_AUTH_WPA2_AES_PSK,
    CYW43_AUTH_WPA3_WPA2_AES_PSK,
    CYW43_AUTH_WPA2_MIXED_PSK,
    CYW43_AUTH_WPA_TKIP_PSK,
    CYW43_AUTH_OPEN,
};

void mode_upload_run(void) {
    device_config_t config;
    if (!storage_load_config(&config) || !config.valid) {
        // 從未完成過熱點設定，沒有 WiFi 帳密可用，放棄本次上傳直接回 BLE 接收模式。
        printf("[UPLOAD] no device config saved yet, skipping.\n");
        led_status_set(LED_ERROR_BURST);
        sleep_ms(1000);
        return;
    }

    printf("[UPLOAD] connecting to WiFi \"%s\"...\n", config.wifi_ssid);
    cyw43_arch_enable_sta_mode();

    int connect_result = PICO_ERROR_GENERIC;
    size_t num_modes = sizeof(WIFI_AUTH_MODES_TO_TRY) / sizeof(WIFI_AUTH_MODES_TO_TRY[0]);
    for (size_t i = 0; i < num_modes; i++) {
        printf("[UPLOAD] trying auth mode 0x%08x...\n", WIFI_AUTH_MODES_TO_TRY[i]);

        // 除錯用：cyw43_arch_wifi_connect_timeout_ms() 只回傳最終成功/失敗，
        // 看不到中間過程。改用 async 版本自己輪詢 cyw43_wifi_link_status()，
        // 才能知道到底是卡在認證（一直停在 JOIN 之前）還是認證過了卡在等
        // DHCP 配 IP（停在 NOIP），這兩種問題完全不同、修法也不一樣。
        int async_err = cyw43_arch_wifi_connect_async(
            config.wifi_ssid, config.wifi_password, WIFI_AUTH_MODES_TO_TRY[i]);
        if (async_err != 0) {
            printf("[UPLOAD]   connect_async failed to start (err=%d)\n", async_err);
            connect_result = async_err;
            cyw43_arch_disable_sta_mode();
            cyw43_arch_enable_sta_mode();
            continue;
        }

        absolute_time_t deadline = make_timeout_time_ms(WIFI_CONNECT_TIMEOUT_MS);
        int last_status = 999;
        connect_result = PICO_ERROR_TIMEOUT;
        while (!time_reached(deadline)) {
            int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status != last_status) {
                printf("[UPLOAD]   link status = %d\n", status);
                last_status = status;
            }

            // 除錯發現 cyw43_wifi_link_status() 有時候不會準時回報 CYW43_LINK_UP，
            // 即使 lwIP 的 DHCP 早就 dhcp_bind() 拿到合法 IP 了——只信任這個狀態
            // 會白白等到逾時，然後 disable/enable_sta_mode() 反而把已經成功的
            // 連線砍掉重來。改成直接檢查 netif 本身是否已經有真實 IP。
            struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
            if (netif_is_up(sta_netif) && !ip4_addr_isany_val(*netif_ip4_addr(sta_netif))) {
                printf("[UPLOAD]   netif has IP %s, treating as connected.\n",
                       ip4addr_ntoa(netif_ip4_addr(sta_netif)));
                connect_result = 0;
                break;
            }
            if (status == CYW43_LINK_UP) {
                connect_result = 0;
                break;
            }
            if (status < 0) {
                connect_result = PICO_ERROR_CONNECT_FAILED;
                break;
            }
            sleep_ms(250);
        }

        if (connect_result == 0) {
            break;
        }
        cyw43_arch_disable_sta_mode();
        cyw43_arch_enable_sta_mode();
    }

    if (connect_result != 0) {
        printf("[UPLOAD] WiFi connect failed after trying all auth modes (rc=%d)\n", connect_result);
        led_status_set(LED_ERROR_BURST);
        sleep_ms(1000);
        cyw43_arch_disable_sta_mode();
        return;
    }

    vital_record_t batch[MAX_BATCH_SIZE];
    size_t count = storage_pending_records(batch, MAX_BATCH_SIZE);
    printf("[UPLOAD] WiFi connected, %u pending record(s) to upload.\n", (unsigned)count);

    if (count > 0) {
        // 量測時 Pico 還沒連網路，received_at_ms 存的是 boot-relative 的 ms，
        // 沒有意義給人看。趁現在 WiFi 已經連上，跟 NTP 校時一次，把這批要上傳
        // 的紀錄換算成真實世界時間再送出去（校時失敗就照舊傳 boot-relative
        // 值，不影響上傳本身，只是這批資料的時間看起來還是不準）。
        wall_clock_sync(8000);
        for (size_t i = 0; i < count; i++) {
            batch[i].received_at_ms = wall_clock_to_epoch_ms(batch[i].received_at_ms);
        }

        bool success = upload_api_post_batch(config.patient_id, batch, count);
        printf("[UPLOAD] upload_api_post_batch() -> %s\n", success ? "success" : "failed");
        storage_mark_uploaded(count, to_ms_since_boot(get_absolute_time()), success);
    }

    cyw43_arch_disable_sta_mode();
}
