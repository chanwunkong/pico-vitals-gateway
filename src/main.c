#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "led_status.h"
#include "state_machine.h"
#include "storage.h"

#include <stdio.h>

int main(void) {
    stdio_init_all();
    sleep_ms(1500); // 給 USB CDC 序列埠一點時間完成列舉，避免最早的幾行 log 被吃掉
    printf("\n[main] pico_gateway starting...\n");

    if (cyw43_arch_init()) {
        printf("[main] cyw43_arch_init() failed\n");
        // 初始化失敗就無法顯示 LED、也無法用任何無線功能，停在這裡等重新開機/排查。
        while (true) {
            sleep_ms(1000);
        }
    }

    led_status_init();
    storage_init();

    state_machine_run(); // 不會返回
    return 0;
}
