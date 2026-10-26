idf_component_register(
    SRCS
        "cal_almanac.c"
        "cal_lunar_table.c"
    INCLUDE_DIRS
        "include"
)

# 纯 C99、零 IDF 依赖 —— 同一份源码会用宿主编译器再编一遍做黄金向量比对
# （tools/almanac_test.py），板端与 PC 端结果必须一致。
