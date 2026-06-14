/*
 * ThumbyElite — rumble motor driver.
 *
 * GP5 = PWM slice 2 channel B (per the Tiny Game Engine's pin map).
 * Intensity maps to PWM duty; pulses decay slightly over their life so
 * long rumbles feel like an impact rather than a buzz.
 */
#include "craft_rumble.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"

#define RUMBLE_PIN 5
#define RUMBLE_WRAP 1023

static uint  s_slice;
static float s_level;      /* current intensity */
static float s_t_left;
static float s_t_total;

void craft_rumble_init(void) {
    gpio_set_function(RUMBLE_PIN, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(RUMBLE_PIN);
    pwm_set_wrap(s_slice, RUMBLE_WRAP);
    pwm_set_gpio_level(RUMBLE_PIN, 0);
    pwm_set_enabled(s_slice, true);
}

void craft_rumble_pulse(float intensity, float seconds) {
    if (intensity <= 0.0f || seconds <= 0.0f) return;
    if (intensity > 1.0f) intensity = 1.0f;
    /* A stronger or longer pulse takes over; a weaker one is ignored
     * while something bigger is still playing. */
    if (s_t_left > 0.0f && intensity < s_level * 0.7f) return;
    s_level = intensity;
    s_t_left = s_t_total = seconds;
}

void craft_rumble_tick(float dt) {
    if (s_t_left <= 0.0f) {
        pwm_set_gpio_level(RUMBLE_PIN, 0);
        return;
    }
    s_t_left -= dt;
    float k = s_t_left > 0.0f ? (s_t_left / s_t_total) : 0.0f;
    /* Ease-out so impacts thump then fade. */
    float duty = s_level * (0.35f + 0.65f * k);
    pwm_set_gpio_level(RUMBLE_PIN,
                       (uint16_t)(duty * (float)RUMBLE_WRAP));
}
