#!/usr/bin/env bash
# 宿主机单测：从 diaglog 源码提取行解析函数，验证各类日志行的解析结果。
set -euo pipefail
SRC="$(dirname "$0")/../components/services/diaglog/src/espaperplay_diaglog.c"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# 提取 diaglog_char_level 与 diaglog_parse_line（含文档注释前的函数体）。
awk '/^static esp_log_level_t diaglog_char_level/,/^}/' "$SRC" > "$OUT/extracted.c"
echo >> "$OUT/extracted.c"
awk '/^static bool diaglog_parse_line/,/^}/' "$SRC" >> "$OUT/extracted.c"

cat > "$OUT/test.c" <<'EOF'
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
typedef enum { ESP_LOG_NONE = 0, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
               ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
#include "extracted.c"

static int fails = 0;
static void check(const char *line, char want_lvl, const char *want_tag, const char *want_msg) {
    char level = 0, tag[24] = {0};
    const char *msg = NULL;
    bool ok = diaglog_parse_line(line, &level, tag, sizeof(tag), &msg);
    if (!ok) {
        printf("FAIL(parse) %-60s\n", line);
        fails++;
        return;
    }
    if (level != want_lvl || strcmp(tag, want_tag) != 0 || strcmp(msg, want_msg) != 0) {
        printf("FAIL(%c/%s/%s want %c/%s/%s) %s\n", level, tag, msg, want_lvl, want_tag,
               want_msg, line);
        fails++;
    }
}
static void check_no(const char *line) {
    char level = 0, tag[24];
    const char *msg = NULL;
    if (diaglog_parse_line(line, &level, tag, sizeof(tag), &msg)) {
        printf("FAIL(should-not-parse) %s\n", line);
        fails++;
    }
}

int main(void) {
    check("I (12345) ESPaperPlay_MAIN: hello world", 'I', "ESPaperPlay_MAIN", "hello world");
    check("W (1200) WiFi: sta disconnected", 'W', "WiFi", "sta disconnected");
    check("E (99) TOUCH: probe failed: 0x14", 'E', "TOUCH", "probe failed: 0x14");
    check("\x1b[0;32mI (123) TAG: colored", 'I', "TAG", "colored");       /* ANSI 前缀 */
    check("V (1) T: ", 'V', "T", "");                                     /* 空消息 */
    check("I (12345) ESPaperPlay_WEB_CFG: url=/api?x=1)", 'I',
          "ESPaperPlay_WEB_CFG", "url=/api?x=1)");
    check_no("");                                    /* 空行 */
    check_no("    0x3ff1: 01 02 03");                /* 多行消息续行 */
    check_no("Error: bad thing");                    /* 非日志格式文本 */
    check_no("I(123) TAG: no space");                /* 畸形前缀 */
    check_no("just some text");
    if (fails == 0) {
        printf("all parse tests passed\n");
        return 0;
    }
    return 1;
}
EOF
gcc -Wall -Wextra -o "$OUT/test" "$OUT/test.c" && "$OUT/test"
