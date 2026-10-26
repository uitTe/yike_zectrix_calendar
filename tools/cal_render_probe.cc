// 台历页渲染器的宿主探针（PC 侧）。
//
// 把 `cal_render_page()` 对一批用例的输出写成 15000 B 的 1bpp 帧，交给
// tools/cal_render_test.py 与 Python 权威渲染器逐字节比对。
//
// 用例从**文件**读（每行 `YYYY-MM-DD|班次|第1段|第2段`，UTF-8，`#` 注释）——
// ⚠️ 不走命令行参数：Windows 上 argv 会按 ANSI 代码页（GBK）传中文，到程序里
// 已经是非法 UTF-8，字库查不到字形、整串被当缺字跳过，于是所有带中文的用例
// 都会「莫名其妙」地画不出来（2026-10-14 就这么被坑过一次）。
//
// 用法（通常由 tools/cal_render_test.py 驱动）：
//   cal_render_probe <out_dir> <cases.txt>
//
// 本文件依赖 cal_render（零 IDF 依赖）+ cal_almanac（纯 C99），所以能直接用宿主
// gcc/g++ 编。cal_font.cc 需要的 esp_log 由 tools/stub/ 的替身接住。

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "cal_almanac.h"
#include "cal_render.h"

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// 去掉行尾的 \r（CRLF 文件）
void StripCr(std::string* s) {
    while (!s->empty() && (s->back() == '\r' || s->back() == '\n')) {
        s->pop_back();
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: cal_render_probe <out_dir> <cases.txt> [--blank-schedule] [--battery-percent N] [--battery-state N]\n");
        return 2;
    }
    const std::string out_dir = argv[1];
    bool blank_schedule = false;
    bool show_battery = false;
    CalBatteryInput battery = {};
    CalWifiInput wifi = {};
    bool show_wifi = false;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--blank-schedule") {
            blank_schedule = true;
        } else if (option == "--battery-percent" && i + 1 < argc) {
            battery.percent = std::stoi(argv[++i]);
            show_battery = true;
        } else if (option == "--battery-state" && i + 1 < argc) {
            battery.state = static_cast<CalBatteryState>(std::stoi(argv[++i]));
        } else if (option == "--wifi-connected") {
            wifi.connected = true;
            show_wifi = true;
        } else if (option == "--wifi-off") {
            wifi.connected = false;
            show_wifi = true;
        } else if (option == "--wifi-rssi" && i + 1 < argc) {
            wifi.rssi = std::stoi(argv[++i]);
            show_wifi = true;
        } else {
            std::fprintf(stderr, "invalid option: %s\n", argv[i]);
            return 2;
        }
    }

    std::ifstream spec_file(argv[2], std::ios::binary);
    if (!spec_file) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }

    std::vector<uint8_t> fb(CAL_FRAME_BYTES);
    int index = 0;
    std::string line;
    while (std::getline(spec_file, line)) {
        StripCr(&line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::vector<std::string> parts = Split(line, '|');
        if (parts.size() != 4) {
            std::fprintf(stderr, "bad spec: %s\n", line.c_str());
            return 2;
        }
        int year = 0;
        int month = 0;
        int day = 0;
        if (std::sscanf(parts[0].c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
            std::fprintf(stderr, "bad date: %s\n", parts[0].c_str());
            return 2;
        }

        const cal_date date = {year, month, day};
        cal_day_info info;
        cal_day_info_get(&date, &info);

        CalPageInput in = {};
        in.info = &info;
        in.shift = parts[1].c_str();
        in.am = parts[2].c_str();
        in.pm = parts[3].c_str();
        in.blank_schedule = blank_schedule;
        in.battery = show_battery ? &battery : nullptr;
        in.wifi = show_wifi ? &wifi : nullptr;

        CalFrame frame = {fb.data(), CAL_FRAME_WIDTH, CAL_FRAME_HEIGHT};
        cal_render_page(&frame, &in);

        char path[1024];
        std::snprintf(path, sizeof(path), "%s/case_%02d.bin", out_dir.c_str(), index);
        FILE* handle = std::fopen(path, "wb");
        if (handle == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", path);
            return 2;
        }
        std::fwrite(fb.data(), 1, fb.size(), handle);
        std::fclose(handle);

        std::printf("%02d %s shift=[%s] am=[%s] pm=[%s] lunar=[%s] term=[%s] fests=%d\n",
                    index, parts[0].c_str(), parts[1].c_str(), parts[2].c_str(),
                    parts[3].c_str(), info.lunar_cn, info.term, info.fest_count);
        ++index;
    }
    return 0;
}
