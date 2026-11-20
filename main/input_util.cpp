#include "input_util.h"

#include "M5Unified.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int64_t s_ab_start_ms = 0;

int64_t input_now_ms() {
    return esp_timer_get_time() / 1000;
}

void input_update() {
    M5.update();
}

bool input_a_pressed() {
    return M5.BtnA.wasPressed() && !M5.BtnB.isPressed();
}

bool input_b_pressed() {
    return M5.BtnB.wasPressed() && !M5.BtnA.isPressed();
}

bool input_a_down() {
    return M5.BtnA.isPressed();
}

bool input_b_down() {
    return M5.BtnB.isPressed();
}

bool input_ab_held(uint32_t hold_ms) {
    if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
        int64_t now = input_now_ms();
        if (s_ab_start_ms == 0) s_ab_start_ms = now;
        return (now - s_ab_start_ms) >= (int64_t)hold_ms;
    }
    s_ab_start_ms = 0;
    return false;
}

void input_wait_ab_release() {
    while (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
        M5.update();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_ab_start_ms = 0;
}

void input_wait_all_release() {
    input_wait_ab_release();
}
