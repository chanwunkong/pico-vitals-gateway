#include "button_input.h"

#include "hardware/gpio.h"
#include "pico/time.h"

#define KEY0_PIN 2
#define KEY1_PIN 3
#define KEY2_PIN 15

static void init_key_pin(uint32_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void button_input_init(void) {
    init_key_pin(KEY0_PIN);
    init_key_pin(KEY1_PIN);
    init_key_pin(KEY2_PIN);
}

static bool key_is_pressed(uint32_t pin) {
    return !gpio_get(pin); // 按下時接地，低電位為按下
}

// KEY0 的長按（進 AP_CONFIG）、短按（AP_CONFIG 裡取消）故意共用同一組
// 按下/放開時間追蹤狀態，不是各自獨立——這兩個動作是分別在不同模式的迴圈
// 裡呼叫（進入前在 mode_ble_receive.c、取消時在 mode_ap_config.c），不會同一
// 時間點都在跑。共用狀態的好處：長按 3 秒觸發進入 AP_CONFIG 之後，如果
// 使用者手指還沒放開，接手輪詢的 button_input_key0_pressed() 會看到
// s_key0_long_triggered 已經是 true、且按下時間其實早就超過短按門檻，不會
// 把「同一次已經觸發過長按的按住」在放開時誤判成一次新的短按，導致剛進
// AP_CONFIG 就被立刻取消。
static bool s_key0_was_pressed = false;
static bool s_key0_long_triggered = false;
static absolute_time_t s_key0_press_started;

bool button_input_key0_long_press(uint32_t hold_ms) {
    bool pressed = key_is_pressed(KEY0_PIN);
    if (pressed && !s_key0_was_pressed) {
        s_key0_press_started = get_absolute_time();
        s_key0_long_triggered = false;
    }
    s_key0_was_pressed = pressed;

    if (!pressed || s_key0_long_triggered) {
        return false;
    }
    if (absolute_time_diff_us(s_key0_press_started, get_absolute_time()) / 1000 >= (int64_t)hold_ms) {
        s_key0_long_triggered = true;
        return true;
    }
    return false;
}

bool button_input_key0_pressed(uint32_t long_press_threshold_ms) {
    bool pressed = key_is_pressed(KEY0_PIN);
    bool fired = false;
    if (pressed && !s_key0_was_pressed) {
        s_key0_press_started = get_absolute_time();
        s_key0_long_triggered = false;
    } else if (!pressed && s_key0_was_pressed) {
        int64_t held_ms = absolute_time_diff_us(s_key0_press_started, get_absolute_time()) / 1000;
        if (!s_key0_long_triggered && held_ms < (int64_t)long_press_threshold_ms) {
            fired = true;
        }
    }
    s_key0_was_pressed = pressed;
    return fired;
}

// 短按（放開時判定）、長按（按住達門檻時判定）各自獨立追蹤自己的按下/放開
// 時間，不共用內部狀態——見 button_input.h 的說明，兩者都是從同一個
// key_is_pressed(KEY1_PIN) 讀值算起，各自算出來的「這次按下是什麼時候開始
// 的」自然一致，不需要互相協調。
static bool s_key1_was_pressed = false;
static absolute_time_t s_key1_press_started;

bool button_input_key1_pressed(uint32_t long_press_threshold_ms) {
    bool pressed = key_is_pressed(KEY1_PIN);
    bool fired = false;
    if (pressed && !s_key1_was_pressed) {
        s_key1_press_started = get_absolute_time();
    } else if (!pressed && s_key1_was_pressed) {
        int64_t held_ms = absolute_time_diff_us(s_key1_press_started, get_absolute_time()) / 1000;
        if (held_ms < (int64_t)long_press_threshold_ms) {
            fired = true;
        }
    }
    s_key1_was_pressed = pressed;
    return fired;
}

static bool s_key1_long_was_pressed = false;
static bool s_key1_long_triggered = false;
static absolute_time_t s_key1_long_press_started;

bool button_input_key1_long_press(uint32_t hold_ms) {
    bool pressed = key_is_pressed(KEY1_PIN);
    if (pressed && !s_key1_long_was_pressed) {
        s_key1_long_press_started = get_absolute_time();
        s_key1_long_triggered = false;
    }
    s_key1_long_was_pressed = pressed;

    if (!pressed || s_key1_long_triggered) {
        return false;
    }
    if (absolute_time_diff_us(s_key1_long_press_started, get_absolute_time()) / 1000 >= (int64_t)hold_ms) {
        s_key1_long_triggered = true;
        return true;
    }
    return false;
}

static bool s_key2_was_pressed = false;
static absolute_time_t s_key2_press_started;

bool button_input_key2_pressed(uint32_t long_press_threshold_ms) {
    bool pressed = key_is_pressed(KEY2_PIN);
    bool fired = false;
    if (pressed && !s_key2_was_pressed) {
        s_key2_press_started = get_absolute_time();
    } else if (!pressed && s_key2_was_pressed) {
        int64_t held_ms = absolute_time_diff_us(s_key2_press_started, get_absolute_time()) / 1000;
        if (held_ms < (int64_t)long_press_threshold_ms) {
            fired = true;
        }
    }
    s_key2_was_pressed = pressed;
    return fired;
}

bool button_input_key2_is_held(void) {
    return key_is_pressed(KEY2_PIN);
}
