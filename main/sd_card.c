/*
 * microSD on ESP32-S31-Korvo-1: SDMMC slot 0 (IO MUX, fixed pins)
 *   CLK GPIO24, CMD GPIO25, D0 GPIO20, D1 GPIO21, D2 GPIO22, D3 GPIO23
 *   SD_CTRL GPIO39 (card power/switch control, optional, see Kconfig)
 *
 * The setup follows the ESP-IDF v6.1 examples (storage/sd_card/sdmmc, usb/device/tusb_msc).
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#if CONFIG_BENCH_SD_SPEED_SDR50
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "sd_card.h"

static const char *TAG = "sd";

#define SD_INIT_RETRY_MS    2000

#if CONFIG_BENCH_SD_BUS_WIDTH_1
#define SD_BUS_WIDTH        1
#else
#define SD_BUS_WIDTH        4
#endif

const char *sd_card_mode_name(void)
{
#if CONFIG_BENCH_SD_SPEED_DS
    return "Default Speed 20 MHz";
#elif CONFIG_BENCH_SD_SPEED_HS50
    return "High Speed 50 MHz";
#elif CONFIG_BENCH_SD_SPEED_SDR50
    return "UHS-I SDR50 100 MHz";
#else
    return "High Speed 40 MHz";
#endif
}

static void card_power_cycle(void)
{
#if CONFIG_BENCH_SD_PWR_CTRL
    const int pin = CONFIG_BENCH_SD_PWR_GPIO;
    const int on = CONFIG_BENCH_SD_PWR_ON_LEVEL;

    gpio_set_level(pin, !on);
    gpio_config_t io = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, !on);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_BENCH_SD_PWR_OFF_MS));
    gpio_set_level(pin, on);
    vTaskDelay(pdMS_TO_TICKS(20));  // let the card supply settle
    ESP_LOGI(TAG, "card power cycled with GPIO%d (on level %d)", pin, on);
#endif
}

esp_err_t sd_card_init(sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(out_card, ESP_ERR_INVALID_ARG, TAG, "out_card is NULL");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = CONFIG_BENCH_SD_FREQ_KHZ;
    // Requests with unaligned buffers that do not fit the bounce buffer in msc_ctrl.c are
    // copied by the driver. Copy 32 KB per SD command instead of the default 8 KB.
    host.unaligned_multi_block_rw_max_chunk_size = 64;

#if CONFIG_BENCH_SD_SPEED_SDR50
    // UHS-I: the card is asked for 1.8 V signaling (S18R). After CMD11 the driver switches the
    // SD I/O supply to the on-chip LDO, which is prepared here at 1.8 V.
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    host.io_voltage = 1.8f;
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CONFIG_BENCH_SD_UHS_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr_ctrl), TAG,
                        "on-chip LDO %d for the SD I/O supply", CONFIG_BENCH_SD_UHS_LDO_CHAN);
    host.pwr_ctrl_handle = pwr_ctrl;
#endif

    // Slot 0 pins are fixed (IO MUX): GPIO24/25/20/21/22/23, see SDMMC_SLOT_CONFIG_DEFAULT().
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = SD_BUS_WIDTH;
#if CONFIG_BENCH_SD_SPEED_SDR50
    slot.flags |= SDMMC_SLOT_FLAG_UHS1;
#endif
#if CONFIG_BENCH_SD_INTERNAL_PULLUP
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    card_power_cycle();

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "sdmmc_host_init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "sdmmc_host_init_slot");

    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(sdmmc_card_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, TAG, "no memory for sdmmc_card_t");

    ESP_LOGI(TAG, "slot 0, %d-bit, requested mode: %s", SD_BUS_WIDTH, sd_card_mode_name());
    for (int attempt = 1; ; attempt++) {
        esp_err_t err = sdmmc_card_init(&host, card);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "card init failed (%s), attempt %d. Insert a microSD card or check the slot.",
                 esp_err_to_name(err), attempt);
        vTaskDelay(pdMS_TO_TICKS(SD_INIT_RETRY_MS));
    }

    *out_card = card;
    return ESP_OK;
}

void sd_card_print_info(const sdmmc_card_t *card)
{
    if (!card) {
        printf("SD card: not initialized\n");
        return;
    }
    sdmmc_card_print_info(stdout, card);

    const uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    const int mdt = card->cid.date;   // SD CID MDT: [11:4] years since 2000, [3:0] month
    printf("Capacity: %" PRIu64 " sectors x %d B = %.2f GB (%.2f GiB)\n",
           (uint64_t)card->csd.capacity, card->csd.sector_size,
           bytes / 1e9, bytes / (1024.0 * 1024.0 * 1024.0));
    printf("CID: MID 0x%02x, OID 0x%04x, PNM \"%s\", PRV %d.%d, PSN 0x%08x, MDT %d-%02d\n",
           card->cid.mfg_id, card->cid.oem_id, card->cid.name,
           (card->cid.revision >> 4) & 0xf, card->cid.revision & 0xf,
           (unsigned)card->cid.serial, 2000 + ((mdt >> 4) & 0xff), mdt & 0xf);
    printf("SCR: sd_spec %d, bus widths 0x%x | SSR: AU %" PRIu32 " KB, discard %d\n",
           card->scr.sd_spec, card->scr.bus_width,
           (uint32_t)card->ssr.alloc_unit_kb, card->ssr.discard_support);
    printf("Bus: %d-bit, %s, UHS-I %s, clock %d kHz (card limit %" PRIu32 " kHz)\n",
           1 << card->log_bus_width, sd_card_mode_name(),
           card->is_uhs1 ? "active" : "off",
           card->real_freq_khz, card->max_freq_khz);
}
