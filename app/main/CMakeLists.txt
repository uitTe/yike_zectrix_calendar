option(CALENDAR_RTC_CONSOLE "启用真实 RTC 校时与唤醒验证入口" OFF)
set(calendar_sources "app_main.cc")
if(CALENDAR_RTC_CONSOLE)
    list(APPEND calendar_sources "../../tools/rtc_console.cc")
endif()

idf_component_register(
    SRCS ${calendar_sources}
    INCLUDE_DIRS "."
    REQUIRES
        cal_almanac
        cal_font
        cal_net
        cal_render
        cal_sched
        zectrix_board
        zectrix_epd
        zectrix_demo_ui
        esp_driver_gpio
        esp_timer
    PRIV_REQUIRES esp_driver_usb_serial_jtag json nvs_flash
)
if(CALENDAR_RTC_CONSOLE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CALENDAR_RTC_CONSOLE=1)
endif()
