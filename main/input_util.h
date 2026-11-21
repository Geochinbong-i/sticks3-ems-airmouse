#pragma once

#include <cstdint>

void input_update();
void input_idle_delay(uint32_t screen_on_ms);
int64_t input_now_ms();

bool input_a_pressed();
bool input_b_pressed();
bool input_a_down();
bool input_b_down();

bool input_ab_held(uint32_t hold_ms);
void input_wait_ab_release();
void input_wait_all_release();
