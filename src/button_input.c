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

static bool s_key0_was_pressed = false;
static bool s_key0_triggered = false;
static absolute_time_t s_key0_press_started;

bool button_input_key0_long_press(uint32_t hold_ms) {
    bool pressed = key_is_pressed(KEY0_PIN);
    if (pressed && !s_key0_was_pressed) {
        s_key0_press_started = get_absolute_time();
        s_key0_triggered = false;
    }
    s_key0_was_pressed = pressed;

    if (!pressed || s_key0_triggered) {
        return false;
    }
    if (absolute_time_diff_us(s_key0_press_started, get_absolute_time()) / 1000 >= (int64_t)hold_ms) {
        s_key0_triggered = true;
        return true;
    }
    return false;
}

static bool s_key1_was_pressed = false;

bool button_input_key1_pressed(void) {
    bool pressed = key_is_pressed(KEY1_PIN);
    bool edge = pressed && !s_key1_was_pressed;
    s_key1_was_pressed = pressed;
    return edge;
}

static bool s_key2_was_pressed = false;

bool button_input_key2_pressed(void) {
    bool pressed = key_is_pressed(KEY2_PIN);
    bool edge = pressed && !s_key2_was_pressed;
    s_key2_was_pressed = pressed;
    return edge;
}
