idf_component_register(
    SRCS "cal_render.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        cal_font
        cal_almanac
)

# 只用到 C 语言特性 + 一点点 C++（范围 for / 引用）；C++17 与 app 其他组件一致。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
