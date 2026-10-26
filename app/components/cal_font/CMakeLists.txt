idf_component_register(
    SRCS "cal_font.cc" "cal_font_data.c"
    INCLUDE_DIRS "include"
    REQUIRES log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
