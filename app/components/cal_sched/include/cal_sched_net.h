/* 排班取数的设备侧接口 —— HTTPS 两步调用与缓存落盘。
 *
 * 这是 `cal_sched.h` 的 IDF 半边：两个头文件刻意分开，因为 `cal_sched.h` 与
 * `cal_sched_parse.c` 要在 PC 上单独编译做断言，不能拖进 ESP-IDF 头。
 *
 * 凭据定义在 `cal_net.h`（`CalApiCredentials`）—— 配网流程要写它们，而配网属于
 * cal_net 组件；放在这里会让 cal_net 反向依赖取数组件，形成组件循环。
 *
 * 接口真源 `docs/11-AGC接口调用说明.md`：先 OAuth 换 token，再带 Bearer 调中转云函数。
 */
#ifndef CAL_SCHED_NET_H_
#define CAL_SCHED_NET_H_

#include "cal_net.h"
#include "cal_sched.h"

// 读取本地缓存。没有任何缓存返回 false（out 清空）。
bool CalSchedCacheLoad(CalSchedCache* out);
// 写入本地缓存。序列化或 NVS 写入失败返回 false。
bool CalSchedCacheSave(const CalSchedCache& cache);

struct CalSchedUpdateResult {
    bool fetched = false;          // 本次是否拿到合法响应（≠ 当天有排班）
    bool has_server_time = false;  // 响应头 Date 可用
    bool wifi_connected = false;   // 本次是否连上过 WiFi，用于页面信号格
    int rssi = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;  // 北京时间
    int attempts = 0;              // 实际尝试轮数，1 表示一次就成
    CalSchedCache cache = {};      // 成功时为最新数据；失败时为旧缓存（可能为空）
};

// 一次完整的取数更新：按需连接 WiFi → 换 token → 调云函数 → 写缓存。
// 失败按 docs/03 §2.4 重试：间隔 1 分钟，最多 3 轮；WiFi 关联失败不额外等待
// （关联本身已耗掉超时时间，等待只会白烧电）。
void CalSchedUpdate(const CalWifiCredentials& wifi, const CalApiCredentials& cred,
                    CalSchedUpdateResult* out);

#endif  // CAL_SCHED_NET_H_
