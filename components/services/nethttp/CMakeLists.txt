#
# SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
#
# SPDX-License-Identifier: MIT
#

idf_component_register(
    SRCS "src/nethttp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls
    PRIV_REQUIRES freertos
)
