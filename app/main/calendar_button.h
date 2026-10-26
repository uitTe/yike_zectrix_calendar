#ifndef CALENDAR_BUTTON_H
#define CALENDAR_BUTTON_H

#include <cstdint>

// 按下、松开均去抖动；连续按住至少 3 秒并松开时只触发一次。
class CalendarResetButton {
public:
    bool Update(bool pressed, int64_t now_ms) {
        if (pressed != sampled_) {
            sampled_ = pressed;
            sampled_at_ = now_ms;
        }
        if (sampled_ == stable_ || now_ms - sampled_at_ < 40) {
            return false;
        }
        stable_ = sampled_;
        if (stable_) {
            pressed_at_ = sampled_at_;
            return false;
        }
        return sampled_at_ - pressed_at_ >= 3000;
    }

private:
    bool sampled_ = false;
    bool stable_ = false;
    int64_t sampled_at_ = 0;
    int64_t pressed_at_ = 0;
};

#endif
