/*
 * microSD card on the ESP32-S31-Korvo-1 SDMMC slot 0.
 */
#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SDMMC host (slot 0, IO MUX pins) and the card.
 *
 * Bus width, speed mode and optional power control come from menuconfig
 * ("S31 SD MSC benchmark"). Retries until a card answers.
 */
esp_err_t sd_card_init(sdmmc_card_t **out_card);

/** @brief Print CID/CSD/SCR/SSR details, bus mode and clock. */
void sd_card_print_info(const sdmmc_card_t *card);

/** @brief Speed mode selected in menuconfig, e.g. "High Speed 40 MHz". */
const char *sd_card_mode_name(void);

#ifdef __cplusplus
}
#endif
