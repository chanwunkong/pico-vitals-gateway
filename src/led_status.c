#include "led_status.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

static led_pattern_t s_pattern = LED_OFF;
static bool s_led_on = false;
static absolute_time_t s_next_change;
static int s_burst_step = 0;

static inline void set_led(bool on) {
    s_led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

static inline void schedule_next(uint32_t delay_ms) {
    s_next_change = make_timeout_time_ms(delay_ms);
}

void led_status_init(void) {
    s_pattern = LED_OFF;
    s_burst_step = 0;
    set_led(false);
    s_next_change = get_absolute_time();
}

void led_status_set(led_pattern_t pattern) {
    if (pattern == s_pattern) {
        return;
    }
    s_pattern = pattern;
    s_burst_step = 0;
    s_next_change = get_absolute_time();

    if (pattern == LED_SOLID_ON) {
        set_led(true);
    } else if (pattern == LED_OFF) {
        set_led(false);
    }
}

void led_status_poll(void) {
    if (absolute_time_diff_us(get_absolute_time(), s_next_change) > 0) {
        return; // 還沒到下一次變化的時間
    }

    switch (s_pattern) {
        case LED_OFF:
        case LED_SOLID_ON:
            break; // 靜態燈號，不需輪詢

        case LED_FAST_BLINK:
            set_led(!s_led_on);
            schedule_next(100);
            break;

        case LED_SLOW_BLINK:
            set_led(!s_led_on);
            schedule_next(1000);
            break;

        case LED_UPLOAD_BLINK:
            set_led(!s_led_on);
            schedule_next(150);
            break;

        case LED_HEARTBEAT:
            if (s_led_on) {
                set_led(false);
                schedule_next(1950);
            } else {
                set_led(true);
                schedule_next(50);
            }
            break;

        case LED_ERROR_BURST:
            if (s_burst_step < 6) {
                set_led(!s_led_on);
                schedule_next(80);
                s_burst_step++;
            } else {
                set_led(false);
                schedule_next(1200);
                s_burst_step = 0;
            }
            break;
    }
}
