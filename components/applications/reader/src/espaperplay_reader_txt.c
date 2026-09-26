/* SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 * SPDX-License-Identifier: MIT */
#include "espaperplay_reader_txt.h"
#include <string.h>
#include <strings.h>

/* Streaming decoding and indexing are implemented in parser/reader_text.c. */
bool espaperplay_reader_is_txt(const char *path) {
    const char *extension = path ? strrchr(path, '.') : NULL;
    return extension && !strcasecmp(extension, ".txt");
}
