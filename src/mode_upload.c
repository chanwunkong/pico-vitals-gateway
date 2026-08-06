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

// 血壓計自己回報的量測時間戳（device_measured_key）不保證裝置內部時鐘校時
// 過——電池換過、從沒設定過、韌體預設值都可能讓這個時鐘跟真實時間差很多年。
// 跟 Pico 自己 NTP 校時過的現在時間比對，差距超過這個範圍就不信任裝置時鐘，
// 退回用 Pico 收到 BLE 通知的時間（見 fora_protocol_measured_key_to_epoch_ms()
// 呼叫端的判斷）。這個值待與主持人確認，見 PROJECT_PLAN.md 第 7.3 節。
#define DEVICE_CLOCK_SANITY_WINDOW_MS (7ULL * 24 * 60 * 60 * 1000)

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
        // 量測時 Pico 還沒連網路，received_at_ms 存的是 boot-relative 的 ms，
        // 沒有意義給人看，換算成真實世界時間再送出去（校時失敗就照舊傳
        // boot-relative 值，不影響上傳本身，只是這批資料的時間看起來還是不準）。
        for (size_t i = 0; i < count; i++) {
            bool used_device_clock = false;
            if (batch[i].device_measured_key != 0) {
                // 裝置本身有認證過的量測時間戳（目前只有血壓計，見
                // fora_protocol.h 的說明），比「Pico 收到 BLE 通知的時間」更
                // 準確——裝置量完到被 Pico 連上讀到資料之間可能有延遲，甚至
                // 不需要靠 NTP 校時（這個時間戳跟 wall_clock 完全無關）。但
                // 裝置自己的時鐘不保證校時過（電池換過、從沒設定過、韌體
                // 預設值都可能差好幾年），跟 Pico 已校時過的現在時間比對一下
                // 合理性，差距太大就不信任，退回用 Pico 收到時間。
                uint64_t device_epoch_ms = fora_protocol_measured_key_to_epoch_ms(batch[i].device_measured_key);
                if (wall_clock_is_synced()) {
                    uint64_t now_epoch_ms = wall_clock_to_epoch_ms(to_ms_since_boot(get_absolute_time()));
                    uint64_t diff_ms = device_epoch_ms > now_epoch_ms
                        ? device_epoch_ms - now_epoch_ms : now_epoch_ms - device_epoch_ms;
                    if (diff_ms <= DEVICE_CLOCK_SANITY_WINDOW_MS) {
                        batch[i].received_at_ms = device_epoch_ms;
                        used_device_clock = true;
                    } else {
                        printf("[UPLOAD] device clock for record %u looks wrong (off by %llu ms), "
                               "falling back to Pico's receive time.\n", (unsigned)i,
                               (unsigned long long)diff_ms);
                    }
                } else {
                    // Pico 自己都還沒校時過，沒有基準可以比對合理性，這種
                    // 情況下裝置時間戳是唯一可用的真實時間來源，照樣採用。
                    batch[i].received_at_ms = device_epoch_ms;
                    used_device_clock = true;
                }
            }
            if (!used_device_clock) {
                batch[i].received_at_ms = wall_clock_to_epoch_ms(batch[i].received_at_ms);
            }
        }

        bool success = upload_api_post_batch(config.patient_id, config.upload_server_host,
                                              config.upload_api_key, batch, count);
        printf("[UPLOAD] upload_api_post_batch() -> %s\n", success ? "success" : "failed");
        storage_mark_uploaded(count, to_ms_since_boot(get_absolute_time()), success);

        char result_text[32];
        snprintf(result_text, sizeof(result_text), "%s (%u record%s)",
                 success ? "Success" : "Failed, will retry", (unsigned)count, count == 1 ? "" : "s");
        display_status_show_upload(config.wifi_ssid, result_text);
    }

    cyw43_arch_disable_sta_mode();
}
