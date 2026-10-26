/* 农历年表（200 项自动生成的内部数据）对外声明。
 * 定义在 cal_lunar_table.c（由 tools/gen_lunar_table.py 生成，勿手改）。 */
#ifndef CAL_LUNAR_TABLE_H
#define CAL_LUNAR_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAL_LUNAR_FIRST_YEAR 1900
#define CAL_LUNAR_LAST_YEAR  2099

/* 下标 = 农历年 - 1900。编码见 cal_lunar_table.c 头部注释。 */
extern const uint32_t cal_lunar_info[200];

#ifdef __cplusplus
}
#endif
#endif /* CAL_LUNAR_TABLE_H */
