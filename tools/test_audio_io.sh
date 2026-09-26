#!/usr/bin/env bash
# 宿主机回归：真实 PCM 读写函数 + 模拟 I2S/锁，检查超时单位、边界和部分读取。
set -euo pipefail
SRC="$(dirname "$0")/../components/drivers/audio/src/espaperplay_audio.c"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
awk '/^esp_err_t espaperplay_audio_speaker_write/,/^}/' "$SRC" > "$OUT/io.c"
awk '/^esp_err_t espaperplay_audio_mic_read/,/^}/' "$SRC" >> "$OUT/io.c"
cat > "$OUT/test.c" <<'TEST'
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT };
#define configTICK_RATE_HZ 100
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
static void *s_lock = (void *)1, *s_tx_chan = (void *)2, *s_rx_chan = (void *)3;
static bool s_active = true;
static int s_volume_percent = 100, lock_ok = 1, calls, releases;
static int64_t now_us, io_elapsed_us;
static size_t io_short;
static uint32_t last_timeout, last_ticks;
static int32_t written[512];
static int64_t esp_timer_get_time(void) { return now_us; }
static int xSemaphoreTake(void *lock, uint32_t ticks) {
    assert(lock == s_lock); last_ticks = ticks; return lock_ok;
}
static void xSemaphoreGive(void *lock) { assert(lock == s_lock); releases++; }
static esp_err_t i2s_channel_write(void *chan, const void *src, size_t n,
                                   size_t *done, uint32_t timeout) {
    assert(chan == s_tx_chan && n <= sizeof(written));
    memcpy(written, src, n); *done = n - io_short;
    last_timeout = timeout; now_us += io_elapsed_us; calls++; return ESP_OK;
}
static esp_err_t i2s_channel_read(void *chan, void *dst, size_t n,
                                  size_t *done, uint32_t timeout) {
    assert(chan == s_rx_chan);
    int32_t *p = dst;
    for (size_t i = 0; i < n / sizeof(*p); i += 2) {
        p[i] = INT32_MIN; p[i + 1] = INT32_MAX;
    }
    *done = n - io_short; last_timeout = timeout;
    now_us += io_elapsed_us; calls++; return ESP_OK;
}
#include "io.c"
static void reset(void) {
    s_active = true; lock_ok = 1; calls = releases = 0;
    now_us = io_elapsed_us = 0; io_short = 0; s_volume_percent = 100;
}
int main(void) {
    int16_t input[512] = { INT16_MIN, INT16_MAX, -1, 0, 1 }, output[256];
    reset();
    assert(espaperplay_audio_speaker_write(input, 5, 100) == ESP_OK);
    assert(last_timeout == 100 && last_ticks == 10 && releases == 1);
    assert(written[0] == INT32_MIN && written[1] == INT32_MIN);
    assert(written[2] == 2147418112 && written[4] == -65536);
    reset(); io_elapsed_us = 101000;
    assert(espaperplay_audio_speaker_write(input, 512, 100) == ESP_ERR_TIMEOUT);
    assert(calls == 1 && releases == 1);
    reset(); lock_ok = 0;
    assert(espaperplay_audio_mic_read(output, 2, 25) == ESP_ERR_TIMEOUT);
    assert(last_ticks == 3 && calls == 0 && releases == 0);
    reset(); io_short = 8;
    assert(espaperplay_audio_mic_read(output, 2, 100) == ESP_ERR_TIMEOUT);
    assert(output[0] == INT16_MIN && last_timeout == 100 && releases == 1);
    reset();
    assert(espaperplay_audio_mic_read(output, 2, 0) == ESP_OK);
    assert(last_timeout == UINT32_MAX && last_ticks == portMAX_DELAY);
    reset(); io_short = 8;
    assert(espaperplay_audio_speaker_write(input, 2, 100) == ESP_ERR_TIMEOUT);
    reset(); s_volume_percent = 0;
    assert(espaperplay_audio_speaker_write(input, 5, 100) == ESP_OK);
    assert(written[0] == 0 && written[2] == 0);
    reset(); s_active = false;
    assert(espaperplay_audio_speaker_write(input, 5, 100) == ESP_ERR_INVALID_STATE);
    assert(espaperplay_audio_mic_read(output, 2, 100) == ESP_ERR_INVALID_STATE);
    assert(espaperplay_audio_mic_read(NULL, 2, 100) == ESP_ERR_INVALID_ARG);
    puts("audio I/O contract: PASS (8 scenarios, UBSan enabled)");
}
TEST
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fsanitize-undefined-trap-on-error \
    -I"$OUT" "$OUT/test.c" -o "$OUT/test"
"$OUT/test"
