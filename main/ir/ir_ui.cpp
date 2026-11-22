#include "ir_ui.h"

#include "input_util.h"
#include "ir_device.h"
#include "ir_store.h"
#include "sound_fx.h"
#include "timer/ems_timer_ui.h"
#include "ui_menu.h"
#include "ui_theme.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kAccent = 0xFF946Au;
constexpr int64_t kLearnTimeoutMs = 8000;
enum class Page { Slots, Saved, Learn, Listening, Captured, Test, Timeout };
enum class TestField { Carrier, Power, Playback, Format, Count, Gap, Send, Save };

const char *const kSlotNames[kIrSlotCount] = {
    "SLOT 01", "SLOT 02", "SLOT 03", "SLOT 04", "SLOT 05", "SLOT 06", "SLOT 07", "SLOT 08"
};
constexpr MenuRow kSaved[] = {
    {"SEND", "Use saved settings", kAccent},
    {"TEST SEND", "Tune and test", kAccent},
    {"RELEARN", "Capture again", kAccent},
};
constexpr MenuRow kLearn[] = {
    {"STANDARD", "Tap remote", kAccent},
    {"EXTENDED", "Keep all frames", kAccent},
};
constexpr MenuRow kCapture[] = {
    {"TEST SEND", "Tune and test", kAccent},
    {"SAVE SIGNAL", "Keep this signal", kAccent},
    {"LEARN AGAIN", "Capture again", kAccent},
};

void line(M5Canvas &canvas, const char *left, const char *right, int y, bool selected) {
    const auto &theme = ui_theme();
    const uint32_t bg = selected ? theme.selected : ((y / 21) % 2 ? theme.alternate : theme.surface);
    canvas.fillRect(3, y, canvas.width() - 6, 20, bg);
    if (selected) canvas.fillRect(3, y, 3, 20, theme.on_selected);
    canvas.setTextColor(selected ? theme.on_selected : theme.text, bg);
    canvas.drawString(left, 9, y + 2);
    canvas.setTextColor(selected ? theme.on_selected : theme.accent, bg);
    canvas.drawRightString(right, canvas.width() - 8, y + 2);
}

TestField test_field(const IrProfile &profile, int index) {
    constexpr TestField normal[] = {TestField::Carrier, TestField::Power, TestField::Playback,
                                   TestField::Format, TestField::Send, TestField::Save};
    return profile.playback == IrPlayback::Repeat ? static_cast<TestField>(index) : normal[index];
}

int test_count(const IrProfile &profile) {
    return profile.playback == IrPlayback::Repeat ? 8 : 6;
}

void draw_tests(M5Canvas &canvas, const IrProfile &profile, int selected, const char *status) {
    const int count = test_count(profile);
    constexpr const char *modes[] = {"Once", "Captured", "Repeat"};
    char khz[10], repeat[4], gap[12];
    std::snprintf(khz, sizeof(khz), "%u kHz", (unsigned)profile.carrier_khz);
    std::snprintf(repeat, sizeof(repeat), "%u", (unsigned)profile.repeat_count);
    std::snprintf(gap, sizeof(gap), "%u ms", (unsigned)profile.repeat_gap_ms);
    const char *labels[] = {"Carrier", "Power", "Playback", "Format", "Send count",
                            "Frame gap", "Test send", "Save settings"};
    const char *values[] = {khz, profile.high_power ? "HIGH" : "NORMAL",
                            modes[static_cast<unsigned>(profile.playback)], profile.nec ? "NEC" : "RAW",
                            repeat, gap, "", ""};
    const bool compact = count == 8 && status;
    const int height = compact ? 20 : 21;
    for (int i = 0; i < count; ++i) {
        const auto field = static_cast<unsigned>(test_field(profile, i));
        line(canvas, labels[field], values[field], 28 + i * height, i == selected);
    }
    if (status) {
        canvas.setTextColor(ui_theme().muted);
        canvas.drawCentreString(status, canvas.width() / 2, compact ? 190 : 170);
        if (count == 6 && std::strncmp(status, "Sent", 4) == 0) {
            canvas.drawCentreString("Check the light", canvas.width() / 2, 190);
        }
    }
}

void draw(M5Canvas &canvas, Page page, const MenuRow *slots, const MenuCursor &slot,
          const MenuCursor &action, const IrSignal &signal, const IrProfile &profile,
          bool extended, const char *status) {
    const auto &theme = ui_theme();
    const auto alarms = ems_timer_active_alarms();
    constexpr const char *titles[] = {"IR REMOTE", "IR SLOT", "LEARN IR",
                                      "LEARN IR", "CAPTURED", "IR TEST", "LEARN IR"};
    canvas.fillScreen(theme.background);
    ui_draw_header(canvas, alarms.any() ? alarms.title() : page == Page::Saved ? kSlotNames[slot.selected] : titles[static_cast<unsigned>(page)],
                   alarms.any() ? 0xB71C1Cu : theme.surface, alarms.any() ? 0xFFFFFFu : theme.text);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);

    switch (page) {
        case Page::Slots: ui_draw_menu(canvas, slots, kIrSlotCount, slot); break;
        case Page::Saved: ui_draw_menu(canvas, kSaved, 3, action); break;
        case Page::Learn: ui_draw_menu(canvas, kLearn, 2, action); break;
        case Page::Listening:
            canvas.fillRoundRect(4, 66, canvas.width() - 8, 82, 4, theme.surface);
            canvas.setTextColor(theme.text);
            canvas.drawCentreString(extended ? "Extended capture" : "Listening", canvas.width() / 2, 78);
            canvas.drawCentreString("Point the remote", canvas.width() / 2, 100);
            canvas.drawCentreString("at the Stick S3", canvas.width() / 2, 120);
            canvas.setTextColor(theme.accent);
            canvas.drawCentreString(extended ? "Hold remote button" : "Press remote once", canvas.width() / 2, 165);
            break;
        case Page::Captured: {
            char frames[8], pulses[8];
            std::snprintf(frames, sizeof(frames), "%u", ir_frame_count(signal));
            std::snprintf(pulses, sizeof(pulses), "%u", (unsigned)signal.count);
            line(canvas, "Mode", extended ? "LONG" : "STANDARD", 31, false);
            line(canvas, "Frames", frames, 52, false);
            line(canvas, "Pulses", pulses, 73, false);
            for (int i = 0; i < 3; ++i) {
                line(canvas, kCapture[i].title, i == 0 || i == 2 ? ">" : "", 113 + 22 * i, action.selected == i);
            }
            break;
        }
        case Page::Test: draw_tests(canvas, profile, action.selected, status); break;
        case Page::Timeout:
            canvas.fillRoundRect(4, 65, canvas.width() - 8, 82, 4, theme.surface);
            canvas.drawRoundRect(4, 65, canvas.width() - 8, 82, 4, theme.border);
            canvas.setTextColor(theme.text);
            canvas.drawCentreString("No IR signal", canvas.width() / 2, 78);
            canvas.setTextColor(theme.muted);
            canvas.drawCentreString("Move the remote", canvas.width() / 2, 100);
            canvas.drawCentreString("closer and retry.", canvas.width() / 2, 122);
            break;
    }

    if (page == Page::Listening) {
        ui_draw_themed_footer(canvas, "Hold A+B Cancel", "");
    } else {
        const char *controls = page == Page::Timeout ? "B Retry" : page == Page::Test ? "A Next   B Select"
                             : page == Page::Learn ? "A Next   B Start" : "A Next   B Open";
        ui_draw_themed_footer(canvas, controls, page != Page::Test && status ? status : "Hold A+B Back");
    }
    ui_present(canvas);
}
} // namespace

void ir_ui_run() {
    M5Canvas canvas(&M5.Display);
    if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) return;

    IrDevice device;
    const bool ready = device.open();
    static IrSignal signal;
    std::array<std::array<char, 22>, kIrSlotCount> subtitles{};
    std::array<MenuRow, kIrSlotCount> slots{};
    IrProfile profile;
    auto default_profile = [] {
        IrProfile p;
        p.high_power = g_settings.ir_tx_high;
        return p;
    };
    for (int i = 0; i < kIrSlotCount; ++i) {
        auto p = default_profile();
        if (ir_load_slot(i, signal, p)) {
            std::snprintf(subtitles[i].data(), subtitles[i].size(), "Saved / %u kHz", (unsigned)p.carrier_khz);
        } else {
            std::snprintf(subtitles[i].data(), subtitles[i].size(), "Empty");
        }
        slots[i] = {kSlotNames[i], subtitles[i].data(), kAccent};
    }

    Page page = Page::Slots;
    Page test_back = Page::Saved;
    MenuCursor slot, action;
    MenuButtons buttons;
    const char *status = ready ? nullptr : "IR unavailable";
    bool extended = false, receiving = false, sending = false;
    int64_t deadline = 0, next_draw = 0;
    unsigned alarm_sequence = 0;
    char sent_status[22]{};

    auto show = [&](Page next, const char *message = nullptr) {
        page = next;
        action = {};
        status = message;
        next_draw = 0;
    };
    auto stop_rx = [&] {
        device.cancel_receive();
        sound_fx_set_ir_listening(false);
        receiving = false;
    };
    auto start_rx = [&](int64_t now) {
        if (ems_timer_active_alarms().any()) {
            status = "EMS alert active";
            return;
        }
        show(Page::Listening);
        deadline = now + kLearnTimeoutMs;
        alarm_sequence = ems_timer_alarm_sequence();
        sound_fx_set_ir_listening(true);
    };
    auto send = [&] {
        sending = device.send(signal, profile);
        status = sending ? "Sending..." : "Test failed";
    };
    auto save = [&] {
        if (!ir_save_slot(slot.selected, signal, profile)) {
            status = "Save failed";
            return;
        }
        auto &label = subtitles[slot.selected];
        std::snprintf(label.data(), label.size(), "Saved / %u kHz", (unsigned)profile.carrier_khz);
        sound_fx_confirm();
        show(Page::Saved, "Saved");
    };

    input_wait_all_release();
    while (true) {
        input_update();
        const auto now = input_now_ms();
        if (page == Page::Listening) {
            if (ems_timer_alarm_sequence() != alarm_sequence || ems_timer_active_alarms().any() || now >= deadline) {
                stop_rx();
                if (now >= deadline) show(Page::Timeout);
                else show(Page::Learn, "EMS alert active");
            } else if (!receiving && sound_fx_ir_ready()) {
                receiving = device.receive(extended);
                if (!receiving) {
                    stop_rx();
                    show(Page::Learn, "RX unavailable");
                }
            } else if (receiving) {
                const auto result = device.poll_receive(signal);
                if (result == IrReceive::Captured) {
                    stop_rx();
                    profile = default_profile();
                    show(Page::Captured);
                    sound_fx_confirm();
                } else if (result == IrReceive::Rejected && !device.receive(extended)) {
                    stop_rx();
                    show(Page::Learn, "RX unavailable");
                }
            }
        }
        if (sending && device.sending_done()) {
            sending = false;
            std::snprintf(sent_status, sizeof(sent_status), profile.high_power && !device.sent_at_high_power()
                          ? "%u kHz Normal" : "Sent at %u kHz", (unsigned)profile.carrier_khz);
            status = sent_status;
            sound_fx_confirm();
            next_draw = 0;
        }

        const auto press = buttons.update(input_a_down(), input_b_down(), now, false);
        if (press == MenuAction::Back && !sending) {
            sound_fx_click();
            if (page == Page::Slots) break;
            if (page == Page::Listening) {
                stop_rx();
                show(Page::Learn);
            } else if (page == Page::Timeout) {
                show(Page::Learn);
            } else if (page == Page::Test) {
                if (test_back == Page::Saved) {
                    profile = default_profile();
                    ir_load_slot(slot.selected, signal, profile);
                }
                show(test_back);
            } else {
                show(Page::Slots);
            }
        } else if (!sending && page != Page::Listening && ready) {
            if (press == MenuAction::Next || press == MenuAction::Previous) {
                const int direction = press == MenuAction::Next ? 1 : -1;
                if (page == Page::Timeout) continue;
                if (page == Page::Slots) slot.move(direction, kIrSlotCount);
                else if (page == Page::Test) action.move(direction, test_count(profile));
                else action.move(direction, page == Page::Learn ? 2 : 3);
                sound_fx_click();
                next_draw = 0;
            } else if (press == MenuAction::Select) {
                status = nullptr;
                next_draw = 0;
                switch (page) {
                    case Page::Slots:
                        profile = default_profile();
                        show(ir_load_slot(slot.selected, signal, profile) ? Page::Saved : Page::Learn);
                        sound_fx_confirm();
                        break;
                    case Page::Saved:
                        if (action.selected == 0) { send(); break; }
                        if (action.selected == 2) { show(Page::Learn); break; }
                        test_back = page;
                        show(Page::Test);
                        break;
                    case Page::Learn:
                        extended = action.selected == 1;
                        start_rx(now);
                        break;
                    case Page::Captured:
                        if (action.selected == 1) { save(); break; }
                        if (action.selected == 2) { show(Page::Learn); break; }
                        test_back = page;
                        show(Page::Test);
                        break;
                    case Page::Test:
                        switch (test_field(profile, action.selected)) {
                            case TestField::Carrier:
                                profile.carrier_khz = profile.carrier_khz == 40 ? 36 : profile.carrier_khz + 2;
                                break;
                            case TestField::Power: profile.high_power = !profile.high_power; break;
                            case TestField::Playback:
                                profile.playback = static_cast<IrPlayback>((static_cast<int>(profile.playback) + 1) % 3);
                                break;
                            case TestField::Format:
                                if (ir_is_nec(signal)) profile.nec = !profile.nec;
                                else status = "RAW signal only";
                                break;
                            case TestField::Count: profile.repeat_count = profile.repeat_count == 2 ? 3 : 2; break;
                            case TestField::Gap:
                                profile.repeat_gap_ms = profile.repeat_gap_ms == 120 ? 40 : profile.repeat_gap_ms + 40;
                                break;
                            case TestField::Send: send(); break;
                            case TestField::Save: save(); break;
                        }
                        break;
                    case Page::Timeout: start_rx(now); break;
                    case Page::Listening: break;
                }
                if (!sending && page != Page::Listening) sound_fx_click();
            }
        }

        if (ui_screen_is_on() && now >= next_draw) {
            draw(canvas, page, slots.data(), slot, action, signal, profile, extended, status);
            next_draw = now + 200;
        }
        input_idle_delay(15);
    }
    stop_rx();
    input_wait_ab_release();
}
