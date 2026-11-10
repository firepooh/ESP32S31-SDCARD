/*
 * USB Mass Storage (TinyUSB on the ESP32-S31 High-Speed OTG port) for the microSD card,
 * card ownership (USB host or application) and transfer statistics.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** VFS path of the card filesystem while the application owns the card. */
#define MSC_BASE_PATH   "/sdcard"

typedef struct {
    uint64_t usb_rd_bytes;      /*!< bytes returned to the host by READ(10) */
    uint64_t usb_wr_bytes;      /*!< bytes accepted from the host by WRITE(10) */
    uint64_t sd_rd_bytes;       /*!< bytes read from the card (host and application) */
    uint64_t sd_wr_bytes;       /*!< bytes written to the card (host and application) */
    uint64_t sd_rd_us;          /*!< time spent in sdmmc_read_sectors() */
    uint64_t sd_wr_us;          /*!< time spent in sdmmc_write_sectors() */
    uint32_t sd_rd_cmds;        /*!< sdmmc_read_sectors() calls */
    uint32_t sd_wr_cmds;        /*!< sdmmc_write_sectors() calls */
    uint32_t sd_rd_err;
    uint32_t sd_wr_err;
    uint32_t bounce_rd;         /*!< unaligned reads copied through the bounce buffer */
    uint32_t bounce_wr;         /*!< unaligned writes copied through the bounce buffer */
    uint32_t host_rejects;      /*!< host READ/WRITE refused while the application owned the card */
} msc_counters_t;

/**
 * @brief Install TinyUSB (High-Speed) and the MSC storage for the card.
 *
 * The filesystem is never formatted automatically.
 *
 * @param card    initialized card
 * @param expose  true: the USB host owns the card at start, false: the application does
 */
esp_err_t msc_ctrl_init(sdmmc_card_t *card, bool expose);

/** @brief Hand the card to the USB host (unmounts the filesystem from /sdcard). */
esp_err_t msc_ctrl_expose(void);

/**
 * @brief Take the card from the USB host and mount the filesystem at /sdcard.
 *
 * Eject the drive on the PC first: the PC may still hold unwritten data in its cache.
 *
 * @param[out] fs_mounted  optional, true if a FAT/exFAT filesystem was mounted
 */
esp_err_t msc_ctrl_take(bool *fs_mounted);

/** @brief Remount the filesystem while the application keeps the card (after format). */
esp_err_t msc_ctrl_remount(bool *fs_mounted);

bool msc_ctrl_host_owns(void);
bool msc_ctrl_fs_mounted(void);
bool msc_ctrl_usb_connected(void);
const char *msc_ctrl_usb_speed_name(void);

void msc_ctrl_get_counters(msc_counters_t *out);
void msc_ctrl_reset_counters(void);

/** @brief Enable/disable the periodic "[msc] RD .. WR .." lines (session summaries stay on). */
void msc_ctrl_set_report(bool on);
bool msc_ctrl_get_report(void);

/** @brief Size of the internal bounce buffer for unaligned SD transfers (0 if not allocated). */
size_t msc_ctrl_bounce_size(void);

#ifdef __cplusplus
}
#endif
