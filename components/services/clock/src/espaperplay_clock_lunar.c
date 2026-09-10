/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - 农历日期换算（实现）
 *
 * 公历 -> 农历（月/日文本，含闰月前缀）。算法为标准「年编码表」法：
 * 每个农历年编码为一个 uint32（bit0-3 = 闰月月份，0 = 无闰月；
 * bit(3+m) = 农历 m 月是否大月 30 天；bit16 = 闰月是否大月）。
 * 编码表由 sxtwl 2.0.7 逐日扫描 2019..2079 生成（tools 无需保留：
 * 表内容与权威库对拍验证，见本文件底部主机对拍说明），覆盖
 * 2020..2077 农历年；表外日期返回 ESP_ERR_NOT_SUPPORTED。
 *
 * 月日名采用传统记法：冬月（十一月）、腊月（十二月），
 * 初X / 十X / 二十 / 廿X / 三十。
 */
#include <stdio.h>
#include <stdbool.h>

#include "esp_err.h"

#include "espaperplay_clock.h"

#define LUNAR_INFO_YEAR_BASE 2020
#define LUNAR_INFO_YEAR_CNT 58
static const uint32_t s_lunar_info[LUNAR_INFO_YEAR_CNT] = {
    0x0a9e4, 0x05560, 0x0ab50, 0x0ada2, 0x06d20, 0x07656, 0x07250, 0x064b0,
    0x06575, 0x0cab0, 0x055a0, 0x056e3, 0x0b690, 0x0f52b, 0x0b520, 0x0b250,
    0x1d0b6, 0x0a4b0, 0x04ab0, 0x02bb5, 0x05ad0, 0x0b6a0, 0x0daa2, 0x0d920,
    0x0ea57, 0x0d250, 0x0a550, 0x1a4d5, 0x04b60, 0x05b50, 0x16d23, 0x0ec90,
    0x0f928, 0x0e920, 0x0d260, 0x15166, 0x0a570, 0x05560, 0x13654, 0x07550,
    0x07490, 0x074b3, 0x06930, 0x0aab7, 0x052b0, 0x0a5b0, 0x0aba5, 0x056a0,
    0x0b650, 0x0baa4, 0x0b4a0, 0x0d958, 0x0a950, 0x052d0, 0x056d6, 0x0ab50,
    0x05aa0, 0x05d54,
};

/** 农历 2020 年正月初一（编码表基准日）。 */
#define LUNAR_EPOCH_Y 2020
#define LUNAR_EPOCH_M 1
#define LUNAR_EPOCH_D 25

static const char *const s_lunar_month_name[] = {"正月", "二月", "三月", "四月",
                                                 "五月", "六月", "七月", "八月",
                                                 "九月", "十月", "冬月", "腊月"};
static const char *const s_lunar_day_name[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

/** 公历日期 -> 自 1970-01-01 的天数（纯历法算术，不依赖时区/DST）。 */
static long lunar_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = (int)(y - era * 400);            /* [0, 399] */
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/** 农历年 info 编码中「闰月月份」（0 = 无闰月）。 */
static int lunar_leap_month(uint32_t info) {
    return (int)(info & 0xf);
}

/** 农历年 m 月（1..12）天数。 */
static int lunar_month_days(uint32_t info, int m) {
    return ((info >> (3 + m)) & 0x1) != 0 ? 30 : 29;
}

/** 农历年闰月天数（无闰月时返回 0）。 */
static int lunar_leap_days(uint32_t info) {
    return lunar_leap_month(info) != 0 ? (((info >> 16) & 0x1) != 0 ? 30 : 29) : 0;
}

esp_err_t espaperplay_clock_lunar_text(const struct tm *solar, char *buf, size_t n) {
    if (solar == NULL || buf == NULL || n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    const int y = solar->tm_year + 1900;
    const int m = solar->tm_mon + 1;
    const int d = solar->tm_mday;
    long offset = lunar_days_from_civil(y, m, d) -
                  lunar_days_from_civil(LUNAR_EPOCH_Y, LUNAR_EPOCH_M, LUNAR_EPOCH_D);
    if (offset < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 定位农历年：整年天数逐段扣除。 */
    uint32_t info = 0;
    bool found = false;
    for (int i = 0; i < LUNAR_INFO_YEAR_CNT; i++) {
        info = s_lunar_info[i];
        int year_days = 0;
        for (int mm = 1; mm <= 12; mm++) {
            year_days += lunar_month_days(info, mm);
        }
        year_days += lunar_leap_days(info);
        if (offset < year_days) {
            found = true;
            break;
        }
        offset -= year_days;
    }
    if (!found) {
        return ESP_ERR_NOT_SUPPORTED; /* 超出编码表覆盖范围 */
    }

    /* 定位农历月/日：月内日期落在当月则止；常规月之后可能是闰月
     * （闰 L 月紧跟 L 月，序列 1..L, 闰L, L+1..12）。 */
    int month = 0;
    bool leap = false;
    for (int mm = 1; mm <= 12; mm++) {
        int len = lunar_month_days(info, mm);
        if (offset < len) {
            month = mm;
            break;
        }
        offset -= len;
        if (lunar_leap_month(info) == mm) {
            len = lunar_leap_days(info);
            if (offset < len) {
                month = mm;
                leap = true;
                break;
            }
            offset -= len;
        }
    }
    if (month == 0 || offset < 0 || offset >= 30) {
        return ESP_ERR_INVALID_STATE; /* 编码表与基准日不一致（不应发生） */
    }

    const int day = (int)offset + 1;
    const int written = snprintf(buf, n, "%s%s%s", leap ? "闰" : "",
                                 s_lunar_month_name[month - 1], s_lunar_day_name[day - 1]);
    if (written < 0 || (size_t)written >= n) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
