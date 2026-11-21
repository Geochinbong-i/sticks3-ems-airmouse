#include "ir_ui.h"

#include "input_util.h"
#include "ir_device.h"
#include "ir_store.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_menu.h"

#include <array>

namespace {
constexpr uint32_t kAccent = 0xFF946A;
constexpr int64_t kLearnTimeoutMs = 8000;

enum class Page { Slots, Listening, Captured, Saved };

constexpr const char *kSlotNames[kIrSlotCount] = {
    "SLOT 01", "SLOT 02", "SLOT 03", "SLOT 04",
    "SLOT 05", "SLOT 06", "SLOT 07", "SLOT 08"
};
constexpr MenuRow kCaptured[] = {
    {"TEST SEND", "Check on device", kAccent},
    {"SAVE", "Keep this signal", kAccent},
    {"RETRY", "Capture again", kAccent},
};
constexpr MenuRow kSaved[] = {
    {"SEND", "Play saved signal", kAccent},
    {"RELEARN", "Capture replacement", kAccent},
};
constexpr MenuRow kListening{"LISTENING", "Press remote once", kAccent};
constexpr int kCapturedCount = std::size(kCaptured);
constexpr int kSavedCount = std::size(kSaved);

const char *signal_label(const IrSignal &signal) {
    return ir_is_nec(signal) ? "NEC / 38 kHz" : "RAW / 38 kHz";
}

void draw_screen(M5Canvas &canvas, Page page, const MenuRow *slots,
                 const MenuCursor &slot, const MenuCursor &action, const char *status) {
    canvas.fillScreen(0x090D14);
    const auto alarms = ems_timer_active_alarms();
    ui_draw_header(canvas, alarms.any() ? alarms.title() : "IR REMOTE",
                   alarms.any() ? 0xB71C1C : 0x142033, 0xFFFFFF);
    switch (page) {
        case Page::Slots: ui_draw_menu(canvas, slots, kIrSlotCount, slot); break;
        case Page::Listening: ui_draw_menu(canvas, &kListening, 1, MenuCursor{-1, 0}); break;
        case Page::Captured: ui_draw_menu(canvas, kCaptured, kCapturedCount, action); break;
        case Page::Saved: ui_draw_menu(canvas, kSaved, kSavedCount, action); break;
    }
    ui_draw_footer2(canvas, page == Page::Listening ? "Point at Stick S3" : "A Next   B OK",
                    status ? status : "Hold A+B Back");
    ui_present(canvas);
}
} // namespace

void ir_ui_run() {
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;

    IrDevice device;
    const bool ready = device.open();
    IrSignal signal;
    std::array<MenuRow, kIrSlotCount> slots{};
    for (int i = 0; i < kIrSlotCount; ++i) {
        slots[i] = {kSlotNames[i], ir_load_slot(i, signal) ? signal_label(signal) : "Empty", kAccent};
    }

    Page page = Page::Slots;
    MenuCursor slot;
    MenuCursor action;
    MenuButtons buttons;
    const char *status = ready ? nullptr : "IR unavailable";
    bool receiving = false;
    bool sending = false;
    int64_t learn_deadline = 0;
    int64_t next_draw = 0;
    unsigned learn_alarm_sequence = 0;

    auto stop_receive = [&] {
        device.cancel_receive();
        sound_fx_set_ir_listening(false);
        receiving = false;
    };
    auto start_receive = [&](int64_t now) {
        if (ems_timer_active_alarms().any()) {
            status = "EMS alert active";
            return;
        }
        action = {};
        page = Page::Listening;
        status = nullptr;
        learn_deadline = now + kLearnTimeoutMs;
        learn_alarm_sequence = ems_timer_alarm_sequence();
        sound_fx_set_ir_listening(true);
    };
    auto end_capture = [&](Page next, const char *message) {
        stop_receive();
        page = next;
        status = message;
        next_draw = 0;
    };
    auto send = [&] {
        sending = device.send(signal);
        status = sending ? "Sending..." : "Send failed";
    };

    input_wait_all_release();
    while (true) {
        input_update();
        const int64_t now = input_now_ms();

        if (page == Page::Listening) {
            if (ems_timer_alarm_sequence() != learn_alarm_sequence || ems_timer_active_alarms().any()) {
                end_capture(Page::Slots, "IR paused: alarm");
            } else if (now >= learn_deadline) {
                end_capture(Page::Slots, "No signal. Retry.");
            } else if (!receiving && sound_fx_ir_ready()) {
                receiving = device.receive();
                if (!receiving) end_capture(Page::Slots, "RX unavailable");
            } else if (receiving) {
                const auto result = device.poll_receive(signal);
                if (result == IrReceive::Captured) {
                    end_capture(Page::Captured, nullptr);
                } else if (result == IrReceive::Rejected && !device.receive()) {
                    end_capture(Page::Slots, "RX unavailable");
                }
            }
        }
        if (sending && device.sending_done()) {
            sending = false;
            status = "Signal sent";
            next_draw = 0;
        }

        const auto press = buttons.update(input_a_down(), input_b_down(), now, false);
        if (press == MenuAction::Back) {
            if (page == Page::Slots) break;
            if (page == Page::Listening) stop_receive();
            page = Page::Slots;
            status = nullptr;
            next_draw = 0;
        } else if (!sending && page != Page::Listening) {
            if (press == MenuAction::Next || press == MenuAction::Previous) {
                const int direction = press == MenuAction::Next ? 1 : -1;
                if (page == Page::Slots) slot.move(direction, kIrSlotCount);
                else action.move(direction, page == Page::Captured ? kCapturedCount : kSavedCount);
                status = ready ? nullptr : "IR unavailable";
                next_draw = 0;
            } else if (press == MenuAction::Select && ready) {
                next_draw = 0;
                if (page == Page::Slots) {
                    if (ir_load_slot(slot.selected, signal)) {
                        action = {};
                        page = Page::Saved;
                        status = nullptr;
                    } else {
                        start_receive(now);
                    }
                } else if (action.selected == 0) {
                    send();
                } else if (page == Page::Captured && action.selected == 1) {
                    if (ir_save_slot(slot.selected, signal)) {
                        slots[slot.selected].subtitle = signal_label(signal);
                        page = Page::Slots;
                        status = "Saved";
                    } else {
                        status = "Save failed";
                    }
                } else {
                    start_receive(now);
                }
            }
        }

        if (ui_screen_is_on() && now >= next_draw) {
            draw_screen(canvas, page, slots.data(), slot, action, status);
            next_draw = now + 200;
        }
        input_idle_delay(15);
    }

    stop_receive();
    input_wait_ab_release();
}
