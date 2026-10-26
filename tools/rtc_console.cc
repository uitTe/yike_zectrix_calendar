// 此入口仅在 CALENDAR_RTC_CONSOLE=ON 的设备验证固件中编译。
// 直接读写真实 PCF8563，随后调用与正式固件相同的每日刷新流程。
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "calendar_clock.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "rtc_pcf8563.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"

void CalendarRtcConsole(RtcPcf8563* rtc) {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));

    char input[256] = {};
    size_t used = 0;
    for (;;) {
        char byte;
        const int count = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(1000));
        if (count == 0) {
            tm now = {};
            char timestamp[32] = "invalid";
            if (rtc->GetTime(now) && CalendarTimeValid(now)) {
                std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &now);
            }
            ESP_LOGI("rtc_console", "READY cause=%d ext1=0x%llx time=%s",
                     static_cast<int>(esp_sleep_get_wakeup_cause()),
                     static_cast<unsigned long long>(esp_sleep_get_ext1_wakeup_status()),
                     timestamp);
            continue;
        }
        ESP_ERROR_CHECK(count == 1 ? ESP_OK : ESP_FAIL);
        if (byte == '\r') {
            continue;
        }
        if (byte != '\n') {
            ESP_ERROR_CHECK(used + 1 < sizeof(input) ? ESP_OK : ESP_ERR_INVALID_SIZE);
            input[used++] = byte;
            continue;
        }
        input[used] = '\0';
        used = 0;
        cJSON* request = cJSON_Parse(input);
        ESP_ERROR_CHECK(cJSON_IsObject(request) ? ESP_OK : ESP_ERR_INVALID_ARG);
        const cJSON* time_value = cJSON_GetObjectItemCaseSensitive(request, "time");
        if (time_value != nullptr) {
            ESP_ERROR_CHECK(cJSON_IsString(time_value) ? ESP_OK : ESP_ERR_INVALID_ARG);
            tm target = {};
            const char* end = strptime(time_value->valuestring, "%Y-%m-%dT%H:%M:%S", &target);
            ESP_ERROR_CHECK(end != nullptr && *end == '\0' && CalendarTimeValid(target)
                                ? ESP_OK : ESP_ERR_INVALID_ARG);
            target.tm_wday = (cal_weekday(target.tm_year + 1900, target.tm_mon + 1,
                                         target.tm_mday) + 1) % 7;
            ESP_ERROR_CHECK(rtc->SetTime(target) ? ESP_OK : ESP_FAIL);
            ESP_LOGI("rtc_console", "SET time=%s", time_value->valuestring);
        }
        const bool run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "run"));
        const bool restart = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "restart"));
        cJSON_Delete(request);
        if (restart) {
            esp_restart();
        }
        if (run) {
            ESP_ERROR_CHECK(usb_serial_jtag_driver_uninstall());
            return;
        }
    }
}
