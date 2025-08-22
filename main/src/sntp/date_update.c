#include <time.h>
#include <string.h>
#include <stdio.h>

#include "vars.h"

#include "date_update.h"

static int last_year = -1, last_month = -1, last_day = -1;
static int last_hour = -1, last_minute = -1;
static int last_weekday = -1;

// 更新UI显示的日期和时间
void date_update()
{
    time_t now = time(NULL);
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    char buffer[32];

    // 日期变更（年月日）
    if (timeinfo.tm_year != last_year || timeinfo.tm_mon != last_month || timeinfo.tm_mday != last_day)
    {
        last_year = timeinfo.tm_year;
        last_month = timeinfo.tm_mon;
        last_day = timeinfo.tm_mday;

        snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        set_var_current_date(buffer);
    }

    // 时间变更（时分）
    if (timeinfo.tm_hour != last_hour || timeinfo.tm_min != last_minute)
    {
        last_hour = timeinfo.tm_hour;
        last_minute = timeinfo.tm_min;

        snprintf(buffer, sizeof(buffer), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        set_var_current_time(buffer);
    }

    // 星期变更
    if (timeinfo.tm_wday != last_weekday)
    {
        last_weekday = timeinfo.tm_wday;

        static const char *weekdays[] = {
            "星期日", "星期一", "星期二", "星期三",
            "星期四", "星期五", "星期六"
        };
        set_var_current_weekday(weekdays[timeinfo.tm_wday]);
    }
}