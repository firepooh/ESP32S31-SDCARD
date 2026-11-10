/*
 * Console commands: info, mount, expose, stats, rawread, filebench, format, restart.
 */
#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cmd_bench_register(sdmmc_card_t *card);

#ifdef __cplusplus
}
#endif
