/* SPDX-License-Identifier: MIT */
#pragma once
#include "reader_parse.h"
typedef struct {
    uint8_t *buf;
    int w, h;
} epub_img_out_t;
esp_err_t rp_image_decode(const char *name, const uint8_t *data, size_t len, int max_w, int max_h,
                          epub_img_out_t *out);
