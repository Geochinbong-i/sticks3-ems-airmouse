#include "input_util.h"
#include "ui_common.h"
#include "time_sync.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int64_t s_ab_start_ms = 0;
static bool s_wake_press = false;

int64_t input_now_ms() {
    return esp_timer_get_time() / 1000;
}

void input_update() {
    M5.update();
    time_sync_poll();
    const bool button_down = M5.BtnA.isPressed() || M5.BtnB.isPressed();
    const bool woke_with_button = ui_update_screen_power(input_now_ms(), button_down);
    s_wake_press = button_down && (s_wake_press || woke_with_button);
}

void input_idle_delay(uint32_t screen_on_ms) {
    vTaskDelay(pdMS_TO_TICKS(ui_screen_is_on() ? screen_on_ms : 45));
}

bool input_a_pressed() {
    return !s_wake_press && M5.BtnA.wasPressed() && !M5.BtnB.isPressed();
}

bool input_b_pressed() {
    return !s_wake_press && M5.BtnB.wasPressed() && !M5.BtnA.isPressed();
}

bool input_a_down() {
    return !s_wake_press && M5.BtnA.isPressed();
}

bool input_b_down() {
    return !s_wake_press && M5.BtnB.isPressed();
}

bool input_ab_held(uint32_t hold_ms) {
    if (input_a_down() && input_b_down()) {
        int64_t now = input_now_ms();
        if (s_ab_start_ms == 0) s_ab_start_ms = now;
        return (now - s_ab_start_ms) >= (int64_t)hold_ms;
    }
    s_ab_start_ms = 0;
    return false;
}

void input_wait_ab_release() {
    while (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
        input_update();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_ab_start_ms = 0;
}

void input_wait_all_release() {
    input_wait_ab_release();
}
