#pragma once

#include <cstdint>

inline constexpr int kMenuVisibleRows = 4;

struct MenuCursor {
    int selected = 0;
    int top = 0;

    void move(int direction, int count) {
        selected = (selected + direction + count) % count;
        if (selected < top) top = selected;
        if (selected >= top + kMenuVisibleRows) top = selected - kMenuVisibleRows + 1;
    }
};

enum class MenuAction { None, Next, Previous, Select, Mute, Back };

class MenuButtons {
public:
    MenuAction update(bool a, bool b, int64_t now_ms, bool at_home) {
        const unsigned down = (a ? 1u : 0u) | (b ? 2u : 0u);
        if (consumed_) {
            if (down == 0) *this = {};
            return MenuAction::None;
        }
        if (down == 0) {
            const unsigned gesture = gesture_;
            *this = {};
            if (gesture == 1) return MenuAction::Next;
            if (gesture == 2) return MenuAction::Select;
            if (gesture == 3) return MenuAction::Previous;
            return MenuAction::None;
        }
        if (gesture_ == 3 && down != 3) return MenuAction::None;
        if (down != gesture_) {
            gesture_ = down;
            started_ms_ = now_ms;
        }
        if (at_home && gesture_ == 1 && now_ms - started_ms_ >= 650) {
            consumed_ = true;
            return MenuAction::Mute;
        }
        if (gesture_ == 3 && now_ms - started_ms_ >= 600) {
            consumed_ = true;
            return at_home ? MenuAction::None : MenuAction::Back;
        }
        return MenuAction::None;
    }

private:
    unsigned gesture_ = 0;
    int64_t started_ms_ = 0;
    bool consumed_ = false;
};
