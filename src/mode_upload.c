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

// 2026-09-01 使用者要求把「怎麼重試都不會成功」情況下卡在連線嘗試的時間壓到
// 1 分鐘以內：5 種認證模式 × 12 秒＝最壞 60 秒，比原本 30 秒/種（最壞 150
// 秒）快很多。合法網路通常第一個對的認證模式幾秒內就會連上（成功是看
// link/IP 狀態，不是等滿這個逾時），這個常數只影響「注定連不上」的失敗路徑
// 要多久才放棄，不影響正常連線的速度。
#define WIFI_CONNECT_TIMEOUT_MS 12000
#define MAX_BATCH_SIZE 32

// WiFi 連線失敗後的退避冷卻期。如果 WiFi 密碼/SSID 設錯這種「怎麼重試都不會
// 成功」的情況完全沒有退避機制，mode_ble_receive.c 的自動觸發條件（NTP 還沒
// 校時成功的定期重試、待傳佇列閒置逾時）會一直把裝置拉回來做這個注定失敗的
// 連線嘗試，掃描 BLE 裝置的時間被壓縮，使用者觀感上就像「卡在 WiFi 連線失敗
// 畫面回不去」。2026-09-01 使用者要求從 5 分鐘拉長到約 1 小時，換取密碼設錯
// 期間裝置能把絕大部分時間留給正常的 BLE 掃描/量測，不要一直做白工的連線
// 嘗試——代價是密碼設錯之後最長要等 1 小時才會再自動重試一次（KEY1 手動觸發
// 不受這個限制，見 mode_ble_receive.c KEY1 分支的說明，想立刻重試可以直接按
// 中鍵）。NTP 重試（mode_ble_receive.c 的 NTP_UNSYNCED_RETRY_MS，5 分鐘）跟
// 這個常數不再刻意對齊——NTP 重試的計時器一樣每 5 分鐘到期，但只要
// mode_upload_in_backoff() 回傳 true 就會被擋下，實際觸發間隔由這裡決定。
#define WIFI_FAILURE_BACKOFF_MS (60 * 60 * 1000)

static absolute_time_t s_backoff_until;
static bool s_in_backoff = false;

bool mode_upload_in_backoff(void) {
    return s_in_backoff && !time_reached(s_backoff_until);
}

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
        s_backoff_until = make_timeout_time_ms(WIFI_FAILURE_BACKOFF_MS);
        s_in_backoff = true;
        led_status_set(LED_ERROR_BURST);
        display_status_show_error("WiFi connect failed, will retry");
        sleep_ms(1000);
        cyw43_arch_disable_sta_mode();
        return;
    }

    // 連線成功了，之前累積的退避狀態（如果有）不用再等，清掉讓下一次失敗
    // 重新起算冷卻期。
    s_in_backoff = false;

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
        // uploadTime 欄位以前是「上傳當下」的真實世界時間，不是個別讀值的量測
        // 時間——2026-08-31 改成每一組各自呼叫 fora_protocol_resolve_epoch_ms()
        // 算出「這組該用的時間」，規則跟畫面顯示完全一樣：D40/MD6 有裝置自己的
        // 量測時間就優先用那個，額溫槍/血氧計沒有裝置時間戳，退回中繼器收到這組
        // 資料當下的時間——不再是「上傳當下」，改掉「出門在外量測、晚上回家才
        // 連線，記錄卻變成晚上的時間」這個問題。校時失敗的話 wall_clock_is_synced()
        // 是 false，upload_api_post_batch() 會整個省略 uploadTime 欄位，不會謊報
        // 一個假的時間戳，這點行為不變。
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
                // 同一組裡的紀錄 source_kind/device_measured_key 都相同（分組依據），
                // 用第一筆的 received_at_ms 代表整組即可；synced 為 false 時這個值
                // 不會被用到（upload_api_post_batch() 看 synced 決定要不要送出欄位）。
                uint64_t group_epoch_ms = synced
                    ? fora_protocol_resolve_epoch_ms(group[0].received_at_ms, group[0].device_measured_key)
                    : 0;
                bool ok = upload_api_post_batch(config.patient_id, config.upload_server_host,
                                                 config.upload_api_key, group, group_count,
                                                 group_epoch_ms, synced, (uint8_t)kind);
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
