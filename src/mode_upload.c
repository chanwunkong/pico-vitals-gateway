#include "mode_upload.h"

#include "common.h"
#include "display_status.h"
#include "fora_protocol.h"
#include "led_status.h"
#include "storage.h"
#include "upload_api.h"
#include "wall_clock.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/netif.h"

#include <stdio.h>

// 30 秒是留給 DHCP 重試＋認證本身的餘裕。
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
        display_status_show_error("No WiFi config saved yet");
        sleep_ms(1000);
        return;
    }

    printf("[UPLOAD] connecting to WiFi \"%s\"...\n", config.wifi_ssid);
    display_status_show_upload(config.wifi_ssid, "Connecting...");
    cyw43_arch_enable_sta_mode();

    int connect_result = PICO_ERROR_GENERIC;
    size_t num_modes = sizeof(WIFI_AUTH_MODES_TO_TRY) / sizeof(WIFI_AUTH_MODES_TO_TRY[0]);
    for (size_t i = 0; i < num_modes; i++) {
        printf("[UPLOAD] trying auth mode 0x%08x...\n", WIFI_AUTH_MODES_TO_TRY[i]);

        // 用 async 版本自己輪詢 cyw43_wifi_link_status()，可以區分是卡在認證
        // （停在 JOIN 之前）還是認證過了卡在等 DHCP 配 IP（停在 NOIP）。
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

            // cyw43_wifi_link_status() 有時候不會準時回報 CYW43_LINK_UP，即使
            // lwIP 的 DHCP 早就拿到合法 IP，所以另外直接檢查 netif 是否已經有
            // 真實 IP，兩個條件任一成立就視為已連線。
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
        display_status_show_error("WiFi connect failed, will retry");
        sleep_ms(1000);
        cyw43_arch_disable_sta_mode();
        return;
    }

    vital_record_t batch[MAX_BATCH_SIZE];
    size_t count = storage_pending_records(batch, MAX_BATCH_SIZE);
    printf("[UPLOAD] WiFi connected, %u pending record(s) to upload.\n", (unsigned)count);

    // 不管這次有沒有資料要傳，只要 WiFi 已經連上就順便嘗試一次 NTP 校時——
    // KEY1 手動觸發（見 mode_ble_receive.c）就算佇列是空的也會走到這裡，
    // 讓使用者能確認網路時間校得準不準，不是只有「有資料要傳」才做。
    wall_clock_sync(8000);

    if (count == 0) {
        display_status_show_upload(config.wifi_ssid, "Connected, nothing to send");
    } else {
        // 後端一次上傳是一筆彙整過的紀錄，一個時間點只能有一個值、dataSource
        // 也只能標示一種醫材來源（見 upload_api.c 開頭的說明），待傳批次要先
        // 依裝置種類（source_kind）分組，同一個裝置種類裡再依
        // device_measured_key（裝置回報的量測時間戳）細分——額溫槍/血氧計沒有
        // 裝置時間戳（恆為 0），細分後全部落在同一組，行為跟以前一樣；血壓計/
        // MD6 一次連線可能抓到好幾個不同時間點的記錄（例如 MD6 往回翻頁抓到
        // 好幾次測試各自的血糖值），細分後才會各自送一次上傳請求，不會像
        // 以前那樣同一個 type 好幾筆值被硬塞進同一個 JSON、只有最後一筆送得
        // 出去（見 PROJECT_PLAN.md 第 6.5 節的說明）。
        // uploadTime 是「上傳當下」的真實世界時間，不是個別讀值的量測時間——
        // 校時失敗的話 wall_clock_is_synced() 是 false，upload_api_post_batch()
        // 會整個省略 uploadTime 欄位，不會謊報一個假的時間戳。
        uint64_t upload_time_ms = wall_clock_to_epoch_ms(to_ms_since_boot(get_absolute_time()));
        bool synced = wall_clock_is_synced();

        size_t groups_sent = 0;
        size_t groups_succeeded = 0;
        for (int kind = 0; kind < FORA_DEVICE_KIND_COUNT; kind++) {
            // 先找出這個裝置種類這一輪待傳批次裡出現過哪些不同的
            // device_measured_key。
            uint32_t seen_keys[MAX_BATCH_SIZE];
            size_t seen_key_count = 0;
            for (size_t i = 0; i < count; i++) {
                if (batch[i].source_kind != (uint8_t)kind) {
                    continue;
                }
                uint32_t key = batch[i].device_measured_key;
                bool already_seen = false;
                for (size_t j = 0; j < seen_key_count; j++) {
                    if (seen_keys[j] == key) {
                        already_seen = true;
                        break;
                    }
                }
                if (!already_seen) {
                    seen_keys[seen_key_count++] = key;
                }
            }

            for (size_t sk = 0; sk < seen_key_count; sk++) {
                vital_record_t group[MAX_BATCH_SIZE];
                size_t group_count = 0;
                for (size_t i = 0; i < count; i++) {
                    if (batch[i].source_kind == (uint8_t)kind && batch[i].device_measured_key == seen_keys[sk]) {
                        group[group_count++] = batch[i];
                    }
                }
                groups_sent++;
                bool ok = upload_api_post_batch(config.patient_id, config.upload_server_host,
                                                 config.upload_api_key, group, group_count,
                                                 upload_time_ms, synced, (uint8_t)kind);
                printf("[UPLOAD] upload_api_post_batch() kind=%d measured_key=%u (%u record(s)) -> %s\n",
                       kind, (unsigned)seen_keys[sk], (unsigned)group_count, ok ? "success" : "failed");
                // 依（裝置種類＋時間點）分開標記結果——storage_mark_uploaded_for_group()
                // 只會動到這一組的紀錄，其他還沒處理到的組別不受影響，見該函式的說明。
                storage_mark_uploaded_for_group(
                    (uint8_t)kind, seen_keys[sk], to_ms_since_boot(get_absolute_time()), ok);
                if (ok) {
                    groups_succeeded++;
                }
            }
        }

        // "group(s)" 不是「裝置種類數」——同一種裝置種類現在可能因為有好幾個
        // 不同時間點的記錄而拆成好幾組各自上傳，見上面的說明。
        char result_text[32];
        snprintf(result_text, sizeof(result_text), "%u/%u group(s) OK",
                 (unsigned)groups_succeeded, (unsigned)groups_sent);
        display_status_show_upload(config.wifi_ssid, result_text);
    }

    cyw43_arch_disable_sta_mode();
}
