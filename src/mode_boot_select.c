#include "mode_boot_select.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "led_status.h"

// 官方寫法，出處 pico-examples/picoboard/button/button.c。
// 官方註解：此函式在其他人同時存取 flash 時（例如 core1 或 XIP streamer）不可用，
// 所以只能在開機、BLE/WiFi/core1 都還沒啟動前呼叫。
static bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
    const uint32_t CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                     GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                     IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i) {
        // 等待腳位電位穩定
    }

#if PICO_RP2040
    #define BOOTSEL_CS_BIT (1u << 1)
#else
    #define BOOTSEL_CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool button_state = !(sio_hw->gpio_hi_in & BOOTSEL_CS_BIT);

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                     GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                     IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return button_state;
}

bool mode_boot_select_check(uint32_t window_ms) {
    led_status_set(LED_FAST_BLINK);

    absolute_time_t deadline = make_timeout_time_ms(window_ms);
    while (!time_reached(deadline)) {
        led_status_poll();
        if (get_bootsel_button()) {
            return true;
        }
        sleep_ms(20);
    }
    return false;
}
