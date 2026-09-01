#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "button_input.h"
#include "display_status.h"
#include "led_status.h"
#include "state_machine.h"
#include "storage.h"

#include <stdio.h>

int main(void) {
    stdio_init_all();
    sleep_ms(1500); // 給 USB CDC 序列埠一點時間完成列舉，避免最早的幾行 log 被吃掉
    printf("\n[main] pico_gateway starting...\n");

    // 預設的 cyw43_arch_init() 用 CYW43_COUNTRY_WORLDWIDE，pico-sdk 官方文件
    // 就寫明這個設定會限制可用頻道/發射功率、效能不一定最好；改用實際國別碼。
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_TAIWAN)) {
        printf("[main] cyw43_arch_init() failed\n");
        // 初始化失敗就無法顯示 LED、也無法用任何無線功能，停在這裡等重新開機/排查。
        while (true) {
            sleep_ms(1000);
        }
    }

    led_status_init();
    button_input_init();
    // 開機時按住 KEY2 不放＝清空重來（littlefs 分區整個重新格式化，待傳佇列/
    // 上傳歷史/設定全部歸零），見 storage_factory_reset() 的說明。要在
    // storage_init() 真的掛載/讀取之前檢查，不然清空就沒意義了。
    //
    // 2026-09-02 使用者實機回報這個判斷「沒有成功清空」——原本是單一時間點
    // 讀一次 GPIO，等於要求使用者的手指剛好在程式跑到這一行的那個瞬間
    // （USB 列舉延遲 1.5 秒 + cyw43 韌體初始化之後）已經按著，時機差一點點
    // 就會判定成「沒按」，沒有任何容錯空間。改成主動等待：開機後給使用者
    // 一段視窗，只要在這段時間內偵測到 KEY2 連續按滿
    // FACTORY_RESET_CONFIRM_HOLD_MS，就視為確認要清空——不要求使用者精準
    // 卡在某個瞬間按下，插電源前先按著、或看到裝置開機後幾百毫秒內按下都
    // 涵蓋得到。
    //
    // 2026-09-02 跟按鍵規則統一：所有按鍵的「長按」一律是「按住達 3000ms」
    // （見 mode_ble_receive.c KEY0_ENTER_CONFIG_HOLD_MS/KEY1_LONG_PRESS_HOLD_MS
    // 的說明），這裡的「開機時長按 KEY2」也算一種長按，門檻同樣改成
    // 3000ms；視窗上限相應拉長到 5000ms，留 2 秒容錯空間，不然 3 秒的門檻
    // 塞不進原本 2 秒的視窗、永遠不可能觸發。
    #define FACTORY_RESET_WINDOW_MS 5000
    #define FACTORY_RESET_CONFIRM_HOLD_MS 3000
    {
        absolute_time_t window_deadline = make_timeout_time_ms(FACTORY_RESET_WINDOW_MS);
        absolute_time_t held_since = nil_time;
        bool triggered = false;
        while (!time_reached(window_deadline)) {
            if (button_input_key2_is_held()) {
                if (is_nil_time(held_since)) {
                    held_since = get_absolute_time();
                } else if (absolute_time_diff_us(held_since, get_absolute_time()) / 1000
                           >= FACTORY_RESET_CONFIRM_HOLD_MS) {
                    triggered = true;
                    break;
                }
            } else {
                held_since = nil_time;
            }
            sleep_ms(20);
        }
        if (triggered) {
            printf("[main] KEY2 held at boot, factory-resetting storage...\n");
            storage_factory_reset();
        }
    }
    storage_init();

    // Phase 1 硬體驗證畫面（display_status_show_boot_test()）不在正常開機流程
    // 自動顯示——那是給開發/除錯用的技術性畫面，不是給使用者看的內容，開機後
    // 幾乎立刻就會進 AP_CONFIG 或 BLE_RECEIVE，讓那個真正有意義的畫面接手。
    // 需要重新確認接線/硬體是否正常時，可以暫時手動呼叫這個函式來測。
    display_status_init();

    state_machine_run(); // 不會返回
    return 0;
}
