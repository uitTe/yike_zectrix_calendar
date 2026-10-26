// 验证生产代码中的上键长按判定，使用明确的按下与松开时间。
#include <cassert>
#include <cstdio>

#include "calendar_button.h"

int main() {
    CalendarResetButton button;
    assert(!button.Update(false, 0));
    assert(!button.Update(true, 100));
    assert(!button.Update(true, 139));
    assert(!button.Update(true, 140));
    assert(!button.Update(false, 3099));
    assert(!button.Update(false, 3139));

    assert(!button.Update(true, 4000));
    assert(!button.Update(true, 4040));
    assert(!button.Update(false, 7000));
    assert(!button.Update(false, 7039));
    assert(button.Update(false, 7040));
    assert(!button.Update(false, 8000));

    assert(!button.Update(true, 9000));
    assert(!button.Update(false, 9020));
    assert(!button.Update(true, 9030));
    assert(!button.Update(true, 9070));
    assert(!button.Update(false, 12029));
    assert(!button.Update(false, 12069));

    assert(!button.Update(true, 13000));
    assert(!button.Update(true, 13040));
    assert(!button.Update(false, 16000));
    assert(!button.Update(true, 16020));
    assert(!button.Update(false, 16030));
    assert(button.Update(false, 16070));
    assert(!button.Update(false, 16100));

    CalendarResetButton held_at_boot;
    assert(!held_at_boot.Update(true, 0));
    assert(!held_at_boot.Update(true, 40));
    assert(!held_at_boot.Update(true, 10000));
    assert(!held_at_boot.Update(false, 10020));
    assert(held_at_boot.Update(false, 10060));
    assert(!held_at_boot.Update(false, 10100));
    std::puts("calendar button: short/long press, 2999/3000 ms, debounce, held at boot, one-shot OK");
}
