#include "mode_upload.h"

#include "common.h"
#include "led_status.h"
#include "storage.h"
#include "upload_api.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include <stdio.h>

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define MAX_BATCH_SIZE 32

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

    int connect_result = cyw43_arch_wifi_connect_timeout_ms(
        config.wifi_ssid, config.wifi_password, CYW43_AUTH_WPA2_AES_PSK, WIFI_CONNECT_TIMEOUT_MS);

    if (connect_result != 0) {
        printf("[UPLOAD] WiFi connect failed (rc=%d)\n", connect_result);
        led_status_set(LED_ERROR_BURST);
        sleep_ms(1000);
        cyw43_arch_disable_sta_mode();
        return;
    }

    vital_record_t batch[MAX_BATCH_SIZE];
    size_t count = storage_pending_records(batch, MAX_BATCH_SIZE);
    printf("[UPLOAD] WiFi connected, %u pending record(s) to upload.\n", (unsigned)count);

    if (count > 0) {
        bool success = upload_api_post_batch(batch, count);
        printf("[UPLOAD] upload_api_post_batch() -> %s\n", success ? "success" : "failed");
        storage_mark_uploaded(count, to_ms_since_boot(get_absolute_time()), success);
    }

    cyw43_arch_disable_sta_mode();
}
