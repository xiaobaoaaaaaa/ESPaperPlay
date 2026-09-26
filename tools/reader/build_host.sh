#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="${1:-/tmp/espaperplay-reader-host}"
"${CC:-clang}" -std=c11 -D_GNU_SOURCE -DESPAPERPLAY_READER_EPUB_HOST -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror \
    -Itools/reader/host -Icomponents/applications/reader/include -Icomponents/applications/reader \
    tools/reader/host/driver.c components/applications/reader/src/parser/*.c \
    components/applications/reader/src/espaperplay_reader_epub.c \
    components/applications/reader/src/libs/tjpgd/tjpgd.c -lz -o "$out"
printf '%s\n' "$out"
