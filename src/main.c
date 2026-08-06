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
    storage_init();

    // Phase 1 硬體驗證畫面（display_status_show_boot_test()）不在正常開機流程
    // 自動顯示——那是給開發/除錯用的技術性畫面，不是給使用者看的內容，開機後
    // 幾乎立刻就會進 AP_CONFIG 或 BLE_RECEIVE，讓那個真正有意義的畫面接手。
    // 需要重新確認接線/硬體是否正常時，可以暫時手動呼叫這個函式來測。
    display_status_init();

    state_machine_run(); // 不會返回
    return 0;
}
